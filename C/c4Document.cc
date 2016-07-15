//
//  c4Document.cc
//  CBForest
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#define NOMINMAX
#include "c4Impl.hh"
#include "c4Document.h"
#include "c4Database.h"
#include "c4Private.h"

#include "Database.hh"
#include "Document.hh"
#include "LogInternal.hh"
#include "VersionedDocument.hh"
#include "SecureRandomize.hh"
#include "SecureDigest.hh"
#include "varint.hh"
#include <ctime>
#include <algorithm>

#include <algorithm>

#include <algorithm>

using namespace cbforest;


static const uint32_t kDefaultMaxRevTreeDepth = 20;


struct C4DocumentInternal : public C4Document, c4Internal::InstanceCounted {
    C4Database* _db;
    VersionedDocument _versionedDoc;
    const Revision *_selectedRev;
    alloc_slice _revIDBuf;
    alloc_slice _selectedRevIDBuf;
    alloc_slice _loadedBody;

    C4DocumentInternal(C4Database* database, C4Slice docID)
    :_db(database->retain()),
     _versionedDoc(*_db, docID),
     _selectedRev(NULL)
    {
        init();
    }

    C4DocumentInternal(C4Database *database, Document &&doc)
    :_db(database->retain()),
    _versionedDoc(*_db, std::move(doc)),
    _selectedRev(NULL)
    {
        init();
    }

    ~C4DocumentInternal() {
        _db->release();
    }

    void init() {
        docID = _versionedDoc.docID();
        flags = (C4DocumentFlags)_versionedDoc.flags();
        if (_versionedDoc.exists())
            flags = (C4DocumentFlags)(flags | kExists);
        
        initRevID();
        selectCurrentRevision();
    }

    void initRevID() {
        if (_versionedDoc.revID().size > 0) {
            _revIDBuf = _versionedDoc.revID().expanded();
        } else {
            _revIDBuf = slice::null;
        }
        revID = _revIDBuf;
        sequence = _versionedDoc.sequence();
    }

    bool selectRevision(const Revision *rev, C4Error *outError =NULL) {
        _selectedRev = rev;
        _loadedBody = slice::null;
        if (rev) {
            _selectedRevIDBuf = rev->revID.expanded();
            selectedRev.revID = _selectedRevIDBuf;
            selectedRev.flags = (C4RevisionFlags)rev->flags;
            selectedRev.sequence = rev->sequence;
            selectedRev.body = rev->inlineBody();
            return true;
        } else {
            _selectedRevIDBuf = slice::null;
            selectedRev.revID = slice::null;
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = slice::null;
            recordHTTPError(kC4HTTPNotFound, outError);
            return false;
        }
    }

    bool selectCurrentRevision() {
        if (_versionedDoc.revsAvailable()) {
            return selectRevision(_versionedDoc.currentRevision());
        } else {
            // Doc body (rev tree) isn't available, but we know enough about the current rev:
            _selectedRev = NULL;
            selectedRev.revID = revID;
            selectedRev.sequence = sequence;
            int revFlags = 0;
            if (flags & kExists) {
                revFlags |= kRevLeaf;
                if (flags & kDeleted)
                    revFlags |= kRevDeleted;
                if (flags & kHasAttachments)
                    revFlags |= kRevHasAttachments;
            }
            selectedRev.flags = (C4RevisionFlags)revFlags;
            selectedRev.body = slice::null;
            return true;
        }
    }

    bool revisionsLoaded() const {
        return _versionedDoc.revsAvailable();
    }

    bool loadRevisions(C4Error *outError) {
        if (_versionedDoc.revsAvailable())
            return true;
        try {
            WITH_LOCK(_db);
            _versionedDoc.read();
            _selectedRev = _versionedDoc.currentRevision();
            return true;
        } catchError(outError)
        return false;
    }

    bool loadSelectedRevBody(C4Error *outError) {
        if (!loadRevisions(outError))
            return false;
        if (!_selectedRev)
            return true;
        if (selectedRev.body.buf)
            return true;  // already loaded
        try {
            WITH_LOCK(_db);
            _loadedBody = _selectedRev->readBody();
            selectedRev.body = _loadedBody;
            if (_loadedBody.buf)
                return true;
            recordHTTPError(kC4HTTPGone, outError); // 410 Gone to denote body that's been compacted away
        } catchError(outError);
        return false;
    }

    void updateMeta() {
        _versionedDoc.updateMeta();
        flags = (C4DocumentFlags)(_versionedDoc.flags() | kExists);
        initRevID();
    }

    bool mustBeInTransaction(C4Error *outError) {
        return _db->mustBeInTransaction(outError);
    }

    void save(unsigned maxRevTreeDepth) {
        _versionedDoc.prune(maxRevTreeDepth);
        {
            WITH_LOCK(_db);
            _versionedDoc.save(*_db->transaction());
        }
        sequence = _versionedDoc.sequence();
    }

};

static inline C4DocumentInternal *internal(C4Document *doc) {
    return (C4DocumentInternal*)doc;
}

// This helper function is meant to be wrapped in a transaction
static bool c4doc_setExpirationInternal(C4Database *db, C4Slice docId, uint64_t timestamp, C4Error *outError)
{
    CBFDebugAssert(db->mustBeInTransaction(outError));
    try {
        if (!db->get(docId, KeyStore::kMetaOnly).exists()) {
            recordError(ForestDBDomain, FDB_RESULT_KEY_NOT_FOUND, outError);
            return false;
        }

        CollatableBuilder tsKeyBuilder;
        tsKeyBuilder.beginArray();
        tsKeyBuilder << (double)timestamp;
        tsKeyBuilder << docId;
        tsKeyBuilder.endArray();
        slice tsKey = tsKeyBuilder.data();

        alloc_slice tsValue(SizeOfVarInt(timestamp));
        PutUVarInt((void *)tsValue.buf, timestamp);

        WITH_LOCK(db);

        Transaction *t = db->transaction();
        KeyStore& expiry = db->getKeyStore("expiry");
        KeyStoreWriter writer(expiry, *t);
        Document existingDoc = writer.get(docId);
        if (existingDoc.exists()) {
            // Previous entry found
            if (existingDoc.body().compare(tsValue) == 0) {
                // No change
                return true;
            }

            // Remove old entry
            uint64_t oldTimestamp;
            CollatableBuilder oldTsKey;
            GetUVarInt(existingDoc.body(), &oldTimestamp);
            oldTsKey.beginArray();
            oldTsKey << (double)oldTimestamp;
            oldTsKey << docId;
            oldTsKey.endArray();
            writer.del(oldTsKey);
        }

        if (timestamp == 0) {
            writer.del(tsKey);
            writer.del(docId);
        } else {
            writer.set(tsKey, slice::null);
            writer.set(docId, tsValue);
        }

        return true;
    } catchError(outError);

    return false;
}

namespace c4Internal {
    C4Document* newC4Document(C4Database *db, Document &&doc) {
        // Doesn't need to lock since Document is already in memory
        return new C4DocumentInternal(db, std::move(doc));
    }

    const VersionedDocument& versionedDocument(C4Document* doc) {
        return internal(doc)->_versionedDoc;
    }
}



void c4doc_free(C4Document *doc) {
    delete (C4DocumentInternal*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = new C4DocumentInternal(database, docID);
        if (mustExist && !doc->_versionedDoc.exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        
        
        return doc;
    } catchError(outError);
    return NULL;
}


C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = new C4DocumentInternal(database, database->get(sequence));
        if (!doc->_versionedDoc.exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError)
{
    try {
        auto idoc = internal(doc);
        if (revID.buf) {
            if (!idoc->loadRevisions(outError))
                return false;
            const Revision *rev = idoc->_versionedDoc[revidBuffer(revID)];
            return idoc->selectRevision(rev, outError) && (!withBody || idoc->loadSelectedRevBody(outError));
        } else {
            idoc->selectRevision(NULL);
            return true;
        }
    } catchError(outError);
    return false;
}


bool c4doc_selectCurrentRevision(C4Document* doc)
{
    return internal(doc)->selectCurrentRevision();
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) {
    return internal(doc)->loadSelectedRevBody(outError);
}


bool c4doc_hasRevisionBody(C4Document* doc) {
    try {
        auto idoc = internal(doc);
        if (!idoc->revisionsLoaded()) {
            Warn("c4doc_hasRevisionBody called on doc loaded without kC4IncludeBodies");
        }
        WITH_LOCK(idoc->_db);
        return idoc->_selectedRev && idoc->_selectedRev->isBodyAvailable();
    } catchError(NULL);
    return false;
}


bool c4doc_selectParentRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (!idoc->revisionsLoaded()) {
        Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
    }
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->parent());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (!idoc->revisionsLoaded()) {
        Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
    }
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->next());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->revisionsLoaded()) {
        Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
    }
    auto rev = idoc->_selectedRev;
    if (rev) {
        do {
            rev = rev->next();
        } while (rev && (!rev->isLeaf() || (!includeDeleted && rev->isDeleted())));
    }
    if (!idoc->selectRevision(rev, NULL)) {
        clearError(outError);  // Normal termination, not error
        return false;
    }
    return (!withBody || idoc->loadSelectedRevBody(outError));
}


unsigned c4rev_getGeneration(C4Slice revID) {
    try {
        return revidBuffer(revID).generation();
    }catchError(NULL)
    return 0;
}


#pragma mark - INSERTING REVISIONS


// Internal form of c4doc_insertRevision that takes compressed revID and doesn't check preconditions
static int32_t insertRevision(C4DocumentInternal *idoc,
                              revid encodedRevID,
                              C4Slice body,
                              bool deletion,
                              bool hasAttachments,
                              bool allowConflict,
                              C4Error *outError)
{
    try {
        int httpStatus;
        auto newRev = idoc->_versionedDoc.insert(encodedRevID,
                                                 body,
                                                 deletion,
                                                 hasAttachments,
                                                 idoc->_selectedRev,
                                                 allowConflict,
                                                 httpStatus);
        if (newRev) {
            // Success:
            idoc->updateMeta();
            newRev = idoc->_versionedDoc.get(encodedRevID);
            idoc->selectRevision(newRev);
            return 1;
        } else if (httpStatus == 200) {
            // Revision already exists, so nothing was added. Not an error.
            c4doc_selectRevision(idoc, encodedRevID.expanded(), true, outError);
            return 0;
        }
        recordHTTPError(httpStatus, outError);
    } catchError(outError)
    return -1;
}


int32_t c4doc_insertRevision(C4Document *doc,
                             C4Slice revID,
                             C4Slice body,
                             bool deletion,
                             bool hasAttachments,
                             bool allowConflict,
                             C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    try {
        revidBuffer encodedRevID(revID);  // this can throw!
        return insertRevision(idoc, encodedRevID, body, deletion, hasAttachments, allowConflict,
                              outError);
    } catchError(outError)
    return -1;
}


int32_t c4doc_insertRevisionWithHistory(C4Document *doc,
                                        C4Slice body,
                                        bool deleted,
                                        bool hasAttachments,
                                        const C4Slice history[],
                                        size_t historyCount,
                                        C4Error *outError)
{
    if (historyCount < 1)
        return 0;
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    int32_t commonAncestor = -1;
    try {
        std::vector<revidBuffer> revIDBuffers(historyCount);
        for (size_t i = 0; i < historyCount; i++)
            revIDBuffers[i].parse(history[i]);
        commonAncestor = idoc->_versionedDoc.insertHistory(revIDBuffers,
                                                           body,
                                                           deleted,
                                                           hasAttachments);
        if (commonAncestor >= 0) {
            idoc->updateMeta();
            idoc->selectRevision(idoc->_versionedDoc[revidBuffer(history[0])]);
        } else {
            recordHTTPError(kC4HTTPBadRequest, outError); // must be invalid revision IDs
        }
    } catchError(outError)
    return commonAncestor;
}


int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(outError))
        return -1;
    try {
        int32_t total = idoc->_versionedDoc.purge(revidBuffer(revID));
        if (total > 0) {
            idoc->updateMeta();
            if (idoc->_selectedRevIDBuf == revID)
                idoc->selectRevision(idoc->_versionedDoc.currentRevision());
        }
        return total;
    } catchError(outError)
    return -1;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    slice result = internal(doc)->_versionedDoc.docType().copy();
    return {result.buf, result.size};
}

void c4doc_setType(C4Document *doc, C4Slice docType) {
    internal(doc)->_versionedDoc.setDocType(docType);
}


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return false;
    try {
        idoc->save(maxRevTreeDepth ? maxRevTreeDepth : kDefaultMaxRevTreeDepth);
        return true;
    } catchError(outError)
    return false;
}

bool c4doc_setExpiration(C4Database *db, C4Slice docId, uint64_t timestamp, C4Error *outError)
{
    if (!c4db_beginTransaction(db, outError)) {
        return false;
    }

    bool commit = c4doc_setExpirationInternal(db, docId, timestamp, outError);
    return c4db_endTransaction(db, commit, outError);
}

uint64_t c4doc_getExpiration(C4Database *db, C4Slice docID)
{
    KeyStore &expiryKvs = db->getKeyStore("expiry");
    Document existing = expiryKvs.get(docID);
    if (!existing.exists()) {
        return 0;
    }

    uint64_t timestamp;
    GetUVarInt(existing.body(), &timestamp);
    return timestamp;
}

static alloc_slice createDocUUID() {
#if SECURE_RANDOMIZE_AVAILABLE
    static const char kBase64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                    "0123456789-_";
    const unsigned kLength = 22; // 22 random base64 chars = 132 bits of entropy
    uint8_t r[kLength];
    SecureRandomize({r, sizeof(r)});

    alloc_slice docIDSlice(1+kLength);
    char *docID = (char*)docIDSlice.buf;
    docID[0] = '-';
    for (unsigned i = 0; i < kLength; ++i)
        docID[i+1] = kBase64[r[i] % 64];
    return docIDSlice;
#else
    error::_throw(FDB_RESULT_CRYPTO_ERROR);
#endif
}


static bool sGenerateOldStyleRevIDs = false;


static revidBuffer generateDocRevID(C4Slice body, C4Slice parentRevID, bool deleted) {
#if SECURE_DIGEST_AVAILABLE
    uint8_t digestBuf[20];
    slice digest;
    if (sGenerateOldStyleRevIDs) {
        // Get MD5 digest of the (length-prefixed) parent rev ID, deletion flag, and revision body:
        md5Context ctx;
        md5_begin(&ctx);
        uint8_t revLen = (uint8_t)std::min((unsigned long)parentRevID.size, 255ul);
        if (revLen > 0)     // Intentionally repeat a bug in CBL's algorithm :)
            md5_add(&ctx, &revLen, 1);
        md5_add(&ctx, parentRevID.buf, revLen);
        uint8_t delByte = deleted;
        md5_add(&ctx, &delByte, 1);
        md5_add(&ctx, body.buf, body.size);
        md5_end(&ctx, digestBuf);
        digest = slice(digestBuf, 16);
    } else {
        // SHA-1 digest:
        sha1Context ctx;
        sha1_begin(&ctx);
        uint8_t revLen = (uint8_t)std::min((unsigned long)parentRevID.size, 255ul);
        sha1_add(&ctx, &revLen, 1);
        sha1_add(&ctx, parentRevID.buf, revLen);
        uint8_t delByte = deleted;
        sha1_add(&ctx, &delByte, 1);
        sha1_add(&ctx, body.buf, body.size);
        sha1_end(&ctx, digestBuf);
        digest = slice(digestBuf, 20);
    }

    // Derive new rev's generation #:
    unsigned generation = 1;
    if (parentRevID.buf) {
        revidBuffer parentID(parentRevID);
        generation = parentID.generation() + 1;
    }
    return revidBuffer(generation, digest);
#else
    error::_throw(FDB_RESULT_CRYPTO_ERROR);
#endif
}

C4SliceResult c4doc_generateRevID(C4Slice body, C4Slice parentRevID, bool deleted) {
    slice result = generateDocRevID(body, parentRevID, deleted).expanded().dontFree();
    return {result.buf, result.size};
}

void c4doc_generateOldStyleRevID(bool generateOldStyle) {
    sGenerateOldStyleRevIDs = generateOldStyle;
}

// Finds a document for a Put of a _new_ revision, and selects the existing parent revision.
// After this succeeds, you can call c4doc_insertRevision and then c4doc_save.
C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError)
{
    if (!database->mustBeInTransaction(outError))
        return NULL;
    C4DocumentInternal *idoc = NULL;
    try {
        do {
            alloc_slice newDocID;
            bool isNewDoc = (!docID.buf);
            if (isNewDoc) {
                newDocID = createDocUUID();
                docID = newDocID;
            }

            idoc = new C4DocumentInternal(database, docID);

            if (!isNewDoc && !idoc->loadRevisions(outError))
                break;

            if (parentRevID.buf) {
                // Updating an existing revision; make sure it exists and is a leaf:
                const Revision *rev = idoc->_versionedDoc[revidBuffer(parentRevID)];
                if (!idoc->selectRevision(rev, outError))
                    break;
                else if (!allowConflict && !rev->isLeaf()) {
                    recordHTTPError(kC4HTTPConflict, outError);
                    break;
                }
            } else {
                // No parent revision given:
                if (deleting) {
                    // Didn't specify a revision to delete: NotFound or a Conflict, depending
                    recordHTTPError(idoc->_versionedDoc.exists() ?kC4HTTPConflict :kC4HTTPNotFound,
                                    outError );
                    break;
                }
                // If doc exists, current rev must be in a deleted state or there will be a conflict:
                const Revision *rev = idoc->_versionedDoc.currentRevision();
                if (rev) {
                    if (!rev->isDeleted()) {
                        recordHTTPError(kC4HTTPConflict, outError);
                        break;
                    }
                    // New rev will be child of the tombstone:
                    // (T0D0: Write a horror novel called "Child Of The Tombstone"!)
                    if (!idoc->selectRevision(rev, outError))
                        break;
                }
            }
            return idoc;
        } while (false); // not a real loop; it's just to allow 'break' statements to exit

    } catchError(outError)
    delete idoc;
    return NULL;
}


C4Document* c4doc_put(C4Database *database,
                      const C4DocPutRequest *rq,
                      size_t *outCommonAncestorIndex,
                      C4Error *outError)
{
    if (!database->mustBeInTransaction(outError))
        return NULL;
    int inserted;
    C4Document *doc;
    if (rq->existingRevision) {
        // Existing revision:
        if (rq->docID.size == 0 || rq->historyCount == 0) {
            recordHTTPError(kC4HTTPBadRequest, outError);
            return NULL;
        }
        doc = c4doc_get(database, rq->docID, false, outError);
        if (!doc)
            return NULL;

        inserted = c4doc_insertRevisionWithHistory(doc, rq->body, rq->deletion, rq->hasAttachments,
                                                   rq->history, rq->historyCount, outError);
    } else {
        // New revision:
        C4Slice parentRevID;
        if (rq->historyCount == 1) {
            parentRevID = rq->history[0];
        } else if (rq->historyCount > 1) {
            recordHTTPError(kC4HTTPBadRequest, outError);
            return NULL;
        }
        doc = c4doc_getForPut(database, rq->docID, parentRevID, rq->deletion, rq->allowConflict,
                              outError);
        if (!doc)
            return NULL;

        revidBuffer revID = generateDocRevID(rq->body, doc->selectedRev.revID, rq->deletion);

        inserted = insertRevision(internal(doc), revID, rq->body, rq->deletion, rq->hasAttachments,
                                  rq->allowConflict, outError);
    }

    // Save:
    if (inserted < 0 || (inserted > 0 && rq->save && !c4doc_save(doc, rq->maxRevTreeDepth,
                                                                 outError))) {
        c4doc_free(doc);
        return NULL;
    }
    
    if (outCommonAncestorIndex)
        *outCommonAncestorIndex = inserted;
    return doc;
}
