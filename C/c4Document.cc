//
// c4Document.cc
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "c4Internal.hh"
#include "c4Database.hh"
#include "c4Document.h"
#include "c4Document+Fleece.h"
#include "c4Private.h"

#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "Document.hh"
#include "Database.hh"
#include "LegacyAttachments.hh"
#include "RevTree.hh"   // only for kDefaultRemoteID
#include "SecureRandomize.hh"
#include "FleeceImpl.hh"

using namespace fleece::impl;
using namespace std;


C4Document* c4doc_retain(C4Document *doc) noexcept {
    retain((Document*)doc);
    return doc;
}


void c4doc_release(C4Document *doc) noexcept {
   release((Document*)doc);
}


static C4Document* newDoc(bool mustExist, C4Error *outError,
                          function_ref<Retained<Document>()> cb) noexcept
{
    return tryCatch<C4Document*>(outError, [&]{
        auto doc = cb();
        if (!doc || (mustExist && !doc->exists())) {
            doc = nullptr;
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        }
        return retain(move(doc));
    });
}


C4Document* c4db_getDoc(C4Database *database,
                       C4Slice docID,
                       bool mustExist,
                       C4DocContentLevel content,
                       C4Error *outError) noexcept
{
    return newDoc(mustExist, outError, [=] {
        return database->documentFactory().newDocumentInstance(docID, ContentOption(content));
    });
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError) noexcept
{
    return c4db_getDoc(database, docID, mustExist, kDocGetCurrentRev, outError);
}


C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError) noexcept
{
    return newDoc(true, outError, [=] {
        return database->documentFactory().newDocumentInstance(
                                        database->defaultKeyStore().get(sequence, kEntireBody));
    });
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (asInternal(doc)->selectRevision(revID, withBody))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        return false;
    });
}


bool c4doc_selectCurrentRevision(C4Document* doc) noexcept
{
    return asInternal(doc)->selectCurrentRevision();
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) noexcept {
    return tryCatch<bool>(outError, [&]{
        if (asInternal(doc)->loadSelectedRevBody())
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
        return false;
    });
}


bool c4doc_hasRevisionBody(C4Document* doc) noexcept {
    return tryCatch<bool>(nullptr, [=]{return asInternal(doc)->hasRevisionBody();});
}


C4Slice c4doc_getRevisionBody(C4Document* doc) C4API {
    return asInternal(doc)->getSelectedRevBody();
}


C4StringResult c4doc_getSelectedRevIDGlobalForm(C4Document* doc) C4API {
    return C4StringResult(asInternal(doc)->getSelectedRevIDGlobalForm());
}


C4StringResult c4doc_getRevisionHistory(C4Document* doc,
                                       unsigned maxRevs,
                                       const C4String backToRevs[],
                                       unsigned backToRevsCount) C4API {
    return C4StringResult(asInternal(doc)->getSelectedRevHistory(maxRevs,
                                                                backToRevs, backToRevsCount));
}


bool c4doc_selectParentRevision(C4Document* doc) noexcept {
    return asInternal(doc)->selectParentRevision();
}


bool c4doc_selectNextRevision(C4Document* doc) noexcept {
    return tryCatch<bool>(nullptr, [=]{return asInternal(doc)->selectNextRevision();});
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (asInternal(doc)->selectNextLeafRevision(includeDeleted)) {
            if (withBody)
                asInternal(doc)->loadSelectedRevBody();
            return true;
        }
        clearError(outError); // normal failure
        return false;
    });
}


bool c4doc_selectCommonAncestorRevision(C4Document* doc, C4String rev1, C4String rev2) noexcept {
    return tryCatch<bool>(nullptr, [&]{
        return asInternal(doc)->selectCommonAncestorRevision(rev1, rev2);
    });
}


#pragma mark - REMOTE DATABASE REVISION TRACKING:


static const char * kRemoteDBURLsDoc = "remotes";


C4RemoteID c4db_getRemoteDBID(C4Database *db, C4String remoteAddress, bool canCreate,
                              C4Error *outError) C4API
{
    using namespace fleece;
    bool inTransaction = false;
    auto remoteID = tryCatch<C4RemoteID>(outError, [&]() {
        // Make two passes: In the first, just look up the "remotes" doc and look for an ID.
        // If the ID isn't found, then do a second pass where we either add the remote URL
        // or create the doc from scratch, in a transaction.
        for (int creating = 0; creating <= 1; ++creating) {
            if (creating) {     // 2nd pass takes place in a transaction
                db->beginTransaction();
                inTransaction = true;
            }

            // Look up the doc in the db, and the remote URL in the doc:
            Record doc = db->getRawDocument(toString(kC4InfoStore), slice(kRemoteDBURLsDoc));
            const Dict *remotes = nullptr;
            C4RemoteID remoteID = 0;
            if (doc.exists()) {
                auto body = Value::fromData(doc.body());
                if (body)
                    remotes = body->asDict();
                if (remotes) {
                    auto idObj = remotes->get(remoteAddress);
                    if (idObj)
                        remoteID = C4RemoteID(idObj->asUnsigned());
                }
            }

            if (remoteID > 0) {
                // Found the remote ID!
                return remoteID;
            } else if (!canCreate) {
                break;
            } else if (creating) {
                // Update or create the document, adding the identifier:
                remoteID = 1;
                Encoder enc;
                enc.beginDictionary();
                for (Dict::iterator i(remotes); i; ++i) {
                    auto existingID = i.value()->asUnsigned();
                    if (existingID) {
                        enc.writeKey(i.keyString());            // Copy existing entry
                        enc.writeUInt(existingID);
                        remoteID = max(remoteID, 1 + C4RemoteID(existingID));   // make sure new ID is unique
                    }
                }
                enc.writeKey(remoteAddress);                       // Add new entry
                enc.writeUInt(remoteID);
                enc.endDictionary();
                alloc_slice body = enc.finish();

                // Save the doc:
                db->putRawDocument(toString(kC4InfoStore), slice(kRemoteDBURLsDoc), nullslice, body);
                db->endTransaction(true);
                inTransaction = false;
                return remoteID;
            }
        }
        if (outError)
            *outError = c4error_make(LiteCoreDomain, kC4ErrorNotFound, {});
        return C4RemoteID(0);
    });
    if (inTransaction)
        c4db_endTransaction(db, false, nullptr);
    return remoteID;
}


C4StringResult c4db_getRemoteDBAddress(C4Database *db, C4RemoteID remoteID) C4API {
    using namespace fleece;
    return tryCatch<C4StringResult>(nullptr, [&]{
        Record doc = db->getRawDocument(toString(kC4InfoStore), slice(kRemoteDBURLsDoc));
        if (doc.exists()) {
            auto body = Value::fromData(doc.body());
            if (body) {
                for (Dict::iterator i(body->asDict()); i; ++i) {
                    if (i.value()->asInt() == remoteID)
                        return C4StringResult(i.keyString());
                }
            }
        }
        return C4StringResult{};
    });
}


C4StringResult c4doc_getRemoteAncestor(C4Document *doc, C4RemoteID remoteDatabase) C4API {
    return tryCatch<C4StringResult>(nullptr, [&]{
        return C4StringResult(asInternal(doc)->remoteAncestorRevID(remoteDatabase));
    });
}


bool c4doc_setRemoteAncestor(C4Document *doc, C4RemoteID remoteDatabase, C4String revID,
                             C4Error *outError) C4API
{
    return tryCatch<bool>(outError, [&]{
        asInternal(doc)->setRemoteAncestorRevID(remoteDatabase, revID);
        return true;
    });
}


// LCOV_EXCL_START
bool c4db_markSynced(C4Database *database,
                     C4String docID,
                     C4String revID,
                     C4SequenceNumber sequence,
                     C4RemoteID remoteID,
                     C4Error *outError) noexcept
{
    bool result = false;
    try {
        if (remoteID == RevTree::kDefaultRemoteID) {
            // Shortcut: can set kSynced flag on the record to mark that the current revision is
            // synced to remote #1. But the call will return false if the sequence no longer
            // matches, i.e this revision is no longer current. Then have to take the slow approach.
            if (database->defaultKeyStore().setDocumentFlag(docID, sequence,
                                                            DocumentFlags::kSynced,
                                                            database->transaction())) {
                return true;
            }
        }

        // Slow path: Load the doc and update the remote-ancestor info in the rev tree:
        Retained<Document> doc(asInternal(c4db_getDoc(database, docID, true, kDocGetAll, outError)));
        release(doc.get());     // balances the +1 ref returned by c4doc_get()
        if (!doc)
            return false;
        if (!revID.buf) {
            // Look up revID by sequence, if it wasn't given:
            Assert(sequence != 0);
            do {
                if (doc->selectedRev.sequence == sequence) {
                    revID = doc->selectedRev.revID;
                    break;
                }
            } while (doc->selectNextRevision());
            if (!revID.buf) {
                c4error_return(LiteCoreDomain, kC4ErrorNotFound, slice("Sequence not found"), outError);
                return false;
            }
        }
        doc->setRemoteAncestorRevID(remoteID, revID);
        result = c4doc_save(doc.get(), 9999, outError);       // don't prune anything
    } catchError(outError)
    return result;
}


#pragma mark - SAVING:


char* c4doc_generateID(char *docID, size_t bufferSize) noexcept {
    if (bufferSize < kC4GeneratedIDLength + 1)
        return nullptr;
    static const char kBase64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "0123456789-_";
    uint8_t r[kC4GeneratedIDLength - 1];
    SecureRandomize({r, sizeof(r)});
    docID[0] = '~';
    for (unsigned i = 0; i < sizeof(r); ++i)
        docID[i+1] = kBase64[r[i] % 64];
    docID[kC4GeneratedIDLength] = '\0';
    return docID;
}


static alloc_slice createDocUUID() {
    char docID[kC4GeneratedIDLength + 1];
    return alloc_slice(c4doc_generateID(docID, sizeof(docID)));
}


// Is this a PutRequest that doesn't require a Record to exist already?
static bool isNewDocPutRequest(C4Database *database, const C4DocPutRequest *rq) {
    if (rq->deltaCB)
        return false;
    else if (rq->existingRevision)
        return database->documentFactory().isFirstGenRevID(rq->history[rq->historyCount-1]);
    else
        return rq->historyCount == 0;
}


// Tries to fulfil a PutRequest by creating a new Record. Returns null if one already exists.
static pair<Document*,int> putNewDoc(C4Database *database,
                                     const C4DocPutRequest *rq)
{
    DebugAssert(rq->save, "putNewDoc optimization works only if rq->save is true");
    Record record(rq->docID);
    if (!rq->docID.buf)
        record.setKey(createDocUUID());
    Retained<Document> idoc = database->documentFactory().newDocumentInstance(record);
    int commonAncestorIndex;
    if (rq->existingRevision)
        commonAncestorIndex = idoc->putExistingRevision(*rq, nullptr);
    else
        commonAncestorIndex = idoc->putNewRevision(*rq, nullptr) ? 0 : -1;
    if (commonAncestorIndex < 0)
        idoc = nullptr;
    return {retain(move(idoc)), commonAncestorIndex};
}


// Finds a document for a Put of a _new_ revision, and selects the existing parent revision.
// After this succeeds, you can call c4doc_insertRevision and then c4doc_save.
C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError) noexcept
{
    if (!database->mustBeInTransaction(outError))
        return nullptr;
    try {
        alloc_slice newDocID;
        if (!docID.buf) {
            newDocID = createDocUUID();
            docID = newDocID;
        }

        Retained<Document> idoc = database->documentFactory().newDocumentInstance(docID,
                                                                                  kEntireBody);
        int code = 0;

        if (parentRevID.buf) {
            // Updating an existing revision; make sure it exists and is a leaf:
            if (!idoc->exists())
                code = kC4ErrorNotFound;
            else if (!idoc->selectRevision(parentRevID, false))
                code = allowConflict ? kC4ErrorNotFound : kC4ErrorConflict;
            else if (!allowConflict && !(idoc->selectedRev.flags & kRevLeaf))
                code = kC4ErrorConflict;
        } else {
            // No parent revision given:
            if (deleting) {
                // Didn't specify a revision to delete: NotFound or a Conflict, depending
                code = ((idoc->flags & kDocExists) ?kC4ErrorConflict :kC4ErrorNotFound);
            } else if ((idoc->flags & kDocExists) && !(idoc->selectedRev.flags & kDocDeleted)) {
                // If doc exists, current rev must be a deletion or there will be a conflict:
                code = kC4ErrorConflict;
            }
        }

        if (code)
            c4error_return(LiteCoreDomain, code, {}, outError);
        else
            return retain(move(idoc));

    } catchError(outError)
    return nullptr;
}


C4Document* c4doc_put(C4Database *database,
                      const C4DocPutRequest *rq,
                      size_t *outCommonAncestorIndex,
                      C4Error *outError) noexcept
{
    if (!database->mustBeInTransaction(outError))
        return nullptr;
    if (rq->docID.buf && !Document::isValidDocID(rq->docID)) {
        c4error_return(LiteCoreDomain, kC4ErrorBadDocID, C4STR("Invalid docID"), outError);
        return nullptr;
    }
    if (rq->existingRevision || rq->historyCount > 0)
        if (!checkParam(rq->docID.buf, "Missing docID", outError))
            return nullptr;
    if (rq->existingRevision) {
        if (!checkParam(rq->historyCount > 0, "No history", outError))
            return nullptr;
    } else {
        if (!checkParam(rq->historyCount <= 1, "Too much history", outError))
            return nullptr;
        if (!checkParam(rq->historyCount > 0 || !(rq->revFlags & kRevDeleted),
                        "Can't create a new already-deleted document", outError))
            return nullptr;
        if (rq->remoteDBID != 0)
            error::_throw(error::InvalidParameter,
                          "remoteDBID cannot be used when existingRevision=false");
    }

    int commonAncestorIndex = 0;
    C4Document *doc = nullptr;
    try {
        if (rq->save && isNewDocPutRequest(database, rq)) {
            // As an optimization, write the doc assuming there is no prior record in the db:
            tie(doc, commonAncestorIndex) = putNewDoc(database, rq);
            // If there's already a record, doc will be null, so we'll continue down regular path.
        }
        if (!doc) {
            if (rq->existingRevision) {
                // Insert existing revision:
                doc = c4db_getDoc(database, rq->docID, false, kDocGetAll, outError);
                if (!doc)
                    return nullptr;
                commonAncestorIndex = asInternal(doc)->putExistingRevision(*rq, outError);
                if (commonAncestorIndex < 0) {
                    c4doc_release(doc);
                    return nullptr;
                }

            } else {
                // Create new revision:
                C4Slice parentRevID = kC4SliceNull;
                if (rq->historyCount == 1)
                    parentRevID = rq->history[0];
                bool deletion = (rq->revFlags & kRevDeleted) != 0;
                doc = c4doc_getForPut(database, rq->docID, parentRevID, deletion, rq->allowConflict,
                                      outError);
                if (!doc)
                    return nullptr;
                if (!asInternal(doc)->putNewRevision(*rq, outError)) {
                    c4doc_release(doc);
                    return nullptr;
                }
                commonAncestorIndex = 0;
            }
        }

        Assert(commonAncestorIndex >= 0, "Unexpected conflict in c4doc_put");
        if (outCommonAncestorIndex)
            *outCommonAncestorIndex = commonAncestorIndex;
        return doc;

    } catchError(outError) {
        c4doc_release(doc);
        return nullptr;
    }
}


C4Document* c4doc_create(C4Database *db,
                         C4String docID,
                         C4Slice revBody,
                         C4RevisionFlags revFlags,
                         C4Error *outError) noexcept
{
    C4DocPutRequest rq = {};
    rq.docID = docID;
    rq.body = revBody;
    rq.revFlags = revFlags;
    rq.save = true;
    return c4doc_put(db, &rq, nullptr, outError);
}


C4Document* c4doc_update(C4Document *doc,
                         C4Slice revBody,
                         C4RevisionFlags revFlags,
                         C4Error *outError) noexcept
{
#if 1
    // Disable the optimized path, because it overwrites the doc in the db using the sequence
    // as MVCC. Unfortunately, the kSynced flag and the remote-rev properties are updated by the
    // push replicator without changing the sequence, so they can be overwritten that way. (#478)
    // Instead, for 2.0.0 we're going back to the safe path of read-modify-write used by c4doc_put.
    C4String history[1] = {doc->selectedRev.revID};
    C4DocPutRequest rq = {};
    rq.body = revBody;
    rq.docID = doc->docID;
    rq.revFlags = revFlags;
    rq.allowConflict = false;
    rq.history = history;
    rq.historyCount = 1;
    rq.save = true;

    auto savedDoc = c4doc_put(external(asInternal(doc)->database()), &rq, nullptr, outError);
    if (!savedDoc) {
        if (outError && outError->domain == LiteCoreDomain && outError->code == kC4ErrorNotFound)
            outError->code = kC4ErrorConflict;  // This is what the logic below returns
    }
    return savedDoc;
#else
    auto idoc = asInternal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return nullptr;
    try {
        idoc->database()->validateRevisionBody(revBody);

        // Why copy the document? Because if we modified it in place it would be too awkward to
        // back out the changes if the save failed. Likewise, the caller may need to be able to
        // back out this entire call if the transaction fails to commit, so having the original
        // doc around helps it.
        unique_ptr<Document> newDoc(idoc->copy());
        C4DocPutRequest rq = {};
        rq.body = revBody;
        rq.revFlags = revFlags;
        rq.allowConflict = true;
        rq.save = true;
        if (newDoc->putNewRevision(rq))
            return newDoc.release();
        c4error_return(LiteCoreDomain, kC4ErrorConflict, C4STR("C4Document is out of date"), outError);
    } catchError(outError)
    return nullptr;
#endif
}


bool c4doc_removeRevisionBody(C4Document* doc) noexcept {
    auto idoc = asInternal(doc);
    return idoc->mustBeInTransaction(NULL) && idoc->removeSelectedRevBody();
}


int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError) noexcept
{
    auto idoc = asInternal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    try {
        idoc->loadRevisions();
        return idoc->purgeRevision(revID);
    } catchError(outError)
    return -1;
}


bool c4doc_resolveConflict2(C4Document *doc,
                            C4String winningRevID,
                            C4String losingRevID,
                            FLDict mergedProperties,
                            C4RevisionFlags mergedFlags,
                            C4Error *outError) noexcept
{
    alloc_slice mergedBody;
    if (mergedProperties) {
        auto db = asInternal(doc)->database();
        auto enc = db->sharedFLEncoder();
        FLEncoder_WriteValue(enc, (FLValue)mergedProperties);
        FLError flErr;
        mergedBody = FLEncoder_Finish(enc, &flErr);
        if (!mergedBody) {
            c4error_return(FleeceDomain, flErr, nullslice, outError);
            return false;
        }
    }
    return c4doc_resolveConflict(doc, winningRevID, losingRevID, mergedBody, mergedFlags, outError);
}


bool c4doc_resolveConflict(C4Document *doc,
                           C4String winningRevID,
                           C4String losingRevID,
                           C4Slice mergedBody,
                           C4RevisionFlags mergedFlags,
                           C4Error *outError) noexcept
{
    if (!asInternal(doc)->mustBeInTransaction(outError))
        return false;
    return tryCatch<bool>(outError, [=]{
        asInternal(doc)->resolveConflict(winningRevID, losingRevID, mergedBody, mergedFlags);
        return true;
    });
}


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError) noexcept
{
    auto idoc = asInternal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return false;
    try {
        if (maxRevTreeDepth == 0)
            maxRevTreeDepth = asInternal(doc)->database()->maxRevTreeDepth();
        if (!idoc->save(maxRevTreeDepth)) {
            if (outError)
                *outError = {LiteCoreDomain, kC4ErrorConflict};
            return false;
        }
        return true;
    } catchError(outError)
    return false;
}


#pragma mark - REVISION IDS & FLAGS:


/// Returns true if the two ASCII revIDs are equal (though they may not be byte-for-byte equal.)
bool c4rev_equal(C4Slice rev1, C4Slice rev2) C4API {
    if (slice(rev1) == slice(rev2))
        return true;
    revidBuffer buf1, buf2;
    return buf1.tryParse(rev1) && buf2.tryParse(rev2) && buf1.isEquivalentTo(buf2);
}


unsigned c4rev_getGeneration(C4Slice revID) C4API {
    try {
        return revidBuffer(revID).generation();
    }catchExceptions()
    return 0;
}


C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags) C4API {
    return Document::currentRevFlagsFromDocFlags(docFlags);
}


#pragma mark - FLEECE-SPECIFIC:


using namespace fleece;


FLDict c4doc_getProperties(C4Document* doc) C4API {
    return asInternal(doc)->getSelectedRevRoot();
}


C4Document* c4doc_containingValue(FLValue value) {
    auto doc = VectorDocumentFactory::documentContaining(value);
    if (!doc)
        doc = TreeDocumentFactory::documentContaining(value);
    return doc;
}


FLEncoder c4db_createFleeceEncoder(C4Database* db) noexcept {
    FLEncoder enc = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true);
    FLEncoder_SetSharedKeys(enc, (FLSharedKeys)db->documentKeys());
    return enc;
}


FLEncoder c4db_getSharedFleeceEncoder(C4Database* db) noexcept {
    return db->sharedFLEncoder();
}


C4SliceResult c4db_encodeJSON(C4Database *db, C4Slice jsonData, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        Encoder &enc = db->sharedEncoder();
        JSONConverter jc(enc);
        if (!jc.encodeJSON(jsonData)) {
            c4error_return(FleeceDomain, jc.errorCode(), slice(jc.errorMessage()), outError);
            return C4SliceResult{};
        }
        return C4SliceResult(enc.finish());
    });
}


C4StringResult c4doc_bodyAsJSON(C4Document *doc, bool canonical, C4Error *outError) noexcept {
    return tryCatch<C4StringResult>(outError, [&]{
        return C4StringResult(c4Internal::asInternal(doc)->bodyAsJSON(canonical));
    });
}


FLSharedKeys c4db_getFLSharedKeys(C4Database *db) noexcept {
    return (FLSharedKeys)db->documentKeys();
}


bool c4doc_isOldMetaProperty(C4String prop) noexcept {
    return legacy_attachments::isOldMetaProperty(prop);
}


bool c4doc_hasOldMetaProperties(FLDict doc) noexcept {
    return legacy_attachments::hasOldMetaProperties((Dict*)doc);
}


bool c4doc_getDictBlobKey(FLDict dict, C4BlobKey *outKey) {
    return Document::getBlobKey((const Dict*)dict, *(blobKey*)outKey);
}


bool c4doc_dictIsBlob(FLDict dict, C4BlobKey *outKey) C4API {
    Assert(outKey);
    return Document::dictIsBlob((const Dict*)dict, *(blobKey*)outKey);
}


C4SliceResult c4doc_getBlobData(FLDict flDict, C4BlobStore *blobStore, C4Error *outError) C4API {
    return tryCatch<C4SliceResult>(outError, [&]{
        alloc_slice blob = Document::getBlobData((const Dict*)flDict, (BlobStore*)blobStore);
        if (!blob && outError)
            *outError = {};
        return FLSliceResult(blob);
    });
}


bool c4doc_dictContainsBlobs(FLDict dict) noexcept {
    bool found = false;
    Document::findBlobReferences((Dict*)dict, [&](const Dict*) {
        found = true;
        return false; // to stop search
    });
    return found;
}


bool c4doc_blobIsCompressible(FLDict blobDict) {
    return Document::blobIsCompressible((const Dict*)blobDict);
}


C4SliceResult c4doc_encodeStrippingOldMetaProperties(FLDict doc, FLSharedKeys sk, C4Error *outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(legacy_attachments::encodeStrippingOldMetaProperties((const Dict*)doc,
                                                                                  (SharedKeys*)sk));
    });
}
