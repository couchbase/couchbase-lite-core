//
//  TreeDocument.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/19/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Document.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "Record.hh"
#include "VersionedDocument.hh"
#include "SecureRandomize.hh"
#include "SecureDigest.hh"
#include "varint.hh"
#include <ctime>
#include <algorithm>


namespace c4Internal {

    static const uint32_t kDefaultMaxRevTreeDepth = 20;


    class TreeDocument : public Document {
    public:
        TreeDocument(Database* database, C4Slice docID)
        :Document(database, docID),
         _versionedDoc(database->defaultKeyStore(), docID),
         _selectedRev(nullptr)
        {
            init();
        }


        TreeDocument(Database *database, const Record &doc)
        :Document(database, move(doc)),
         _versionedDoc(database->defaultKeyStore(), doc),
         _selectedRev(nullptr)
        {
            init();
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
                _revIDBuf = nullslice;
            }
            revID = _revIDBuf;
            sequence = _versionedDoc.sequence();
        }

        slice type() noexcept override {
            return _versionedDoc.docType();
        }

        void setType(C4Slice docType) noexcept override {
            _versionedDoc.setDocType(docType);
        }

        const Record& record() override {
            return _versionedDoc.record();
        }

        bool exists() override {
            return _versionedDoc.exists();
        }

        bool revisionsLoaded() const noexcept override {
            return _versionedDoc.revsAvailable();
        }

        void loadRevisions() override {
            if (!_versionedDoc.revsAvailable()) {
                WITH_LOCK(_db);
                _versionedDoc.read();
                _selectedRev = _versionedDoc.currentRevision();
            }
        }

        bool hasRevisionBody() noexcept override {
            if (!revisionsLoaded())
                Warn("c4doc_hasRevisionBody called on doc loaded without kC4IncludeBodies");
            WITH_LOCK(database());
            return _selectedRev && _selectedRev->isBodyAvailable();
        }

        bool loadSelectedRevBodyIfAvailable() override {
            loadRevisions();
            if (_selectedRev && !selectedRev.body.buf) {
                WITH_LOCK(_db);
                _loadedBody = _selectedRev->readBody();
                selectedRev.body = _loadedBody;
                if (!_loadedBody.buf)
                    return false;           // compacted away
            }
            return true;
        }

        bool selectRevision(const Rev *rev) noexcept {   // doesn't throw
            _selectedRev = rev;
            _loadedBody = nullslice;
            if (rev) {
                _selectedRevIDBuf = rev->revID.expanded();
                selectedRev.revID = _selectedRevIDBuf;
                selectedRev.flags = (C4RevisionFlags)rev->flags;
                selectedRev.sequence = rev->sequence;
                selectedRev.body = rev->inlineBody();
                return true;
            } else {
                clearSelectedRevision();
                return false;
            }
        }

        bool selectRevision(C4Slice revID, bool withBody) override {
            if (revID.buf) {
                loadRevisions();
                const Rev *rev = _versionedDoc[revidBuffer(revID)];
                if (!selectRevision(rev))
                    return false;
                if (withBody)
                    loadSelectedRevBody();
            } else {
                selectRevision(nullptr);
            }
            return true;
        }

        bool selectCurrentRevision() noexcept override { // doesn't throw
            if (_versionedDoc.revsAvailable()) {
                selectRevision(_versionedDoc.currentRevision());
                return true;
            } else {
                _selectedRev = nullptr;
                Document::selectCurrentRevision();
                return false;
            }
        }

        bool selectParentRevision() noexcept override {
            if (!revisionsLoaded())
                Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
            if (_selectedRev)
                selectRevision(_selectedRev->parent());
            return _selectedRev != nullptr;
        }

        bool selectNextRevision() noexcept override {    // does not throw
            if (!revisionsLoaded())
                Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
            if (_selectedRev)
                selectRevision(_selectedRev->next());
            return _selectedRev != nullptr;
        }

        bool selectNextLeafRevision(bool includeDeleted) noexcept override {
            if (!revisionsLoaded())
                Warn("Trying to access revision tree of doc loaded without kC4IncludeBodies");
            auto rev = _selectedRev;
            if (!rev)
                return false;
            do {
                rev = rev->next();
                if (!rev)
                    return false;
            } while (!rev->isLeaf() || (!includeDeleted && rev->isDeleted()));
            selectRevision(rev);
            return true;
        }

        void updateMeta() {
            _versionedDoc.updateMeta();
            flags = (C4DocumentFlags)(_versionedDoc.flags() | kExists);
            initRevID();
        }

        void save(unsigned maxRevTreeDepth) {
            _versionedDoc.prune(maxRevTreeDepth);
            {
                WITH_LOCK(_db);
                _versionedDoc.save(_db->transaction());
            }
            selectedRev.flags &= ~kRevNew;
            if (_versionedDoc.sequence() > sequence) {
                sequence = _versionedDoc.sequence();
                _db->saved(this);
            }
        }

        int32_t purgeRevision(C4Slice revID) override {
            int32_t total;
            if (revID.buf)
                total = _versionedDoc.purge(revidBuffer(revID));
            else
                total = _versionedDoc.purgeAll();
            if (total > 0) {
                updateMeta();
                if (_selectedRevIDBuf == revID)
                    selectRevision(_versionedDoc.currentRevision());
            }
            return total;
        }

        virtual int32_t putExistingRevision(const C4DocPutRequest&) override;
        virtual bool putNewRevision(const C4DocPutRequest&) override;
            
        public:
            VersionedDocument _versionedDoc;
            const Rev *_selectedRev;
    };


#pragma mark - FACTORY:


    Document* TreeDocumentFactory::newDocumentInstance(C4Slice docID) {
        return new TreeDocument(database(), docID);
    }

    Document* TreeDocumentFactory::newDocumentInstance(const Record &doc) {
        return new TreeDocument(database(), doc);
    }


    bool TreeDocumentFactory::readDocMeta(const Record &doc,
                                          C4DocumentFlags *outFlags,
                                          alloc_slice *outRevID,
                                          slice *outDocType)
    {
        VersionedDocument::Flags vdocFlags;
        revidBuffer packedRevID;
        slice docType;
        if (!VersionedDocument::readMeta(doc, vdocFlags, packedRevID, docType))
            return false;
        if (outFlags) {
            C4DocumentFlags c4flags = 0;
            if (vdocFlags & VersionedDocument::kDeleted)
                c4flags |= kDeleted;
            if (vdocFlags & VersionedDocument::kConflicted)
                c4flags |= kConflicted;
            if (vdocFlags & VersionedDocument::kHasAttachments)
                c4flags |= kHasAttachments;
            *outFlags = c4flags;
        }
        if (outRevID)
            *outRevID = packedRevID.expanded();
        if (outDocType)
            *outDocType = docType;
        return true;
    }


#pragma mark - INSERTING REVISIONS


    int32_t TreeDocument::putExistingRevision(const C4DocPutRequest &rq) {
        Assert(rq.historyCount >= 1);
        int32_t commonAncestor = -1;
        loadRevisions();
        vector<revidBuffer> revIDBuffers(rq.historyCount);
        for (size_t i = 0; i < rq.historyCount; i++)
            revIDBuffers[i].parse(rq.history[i]);
        commonAncestor = _versionedDoc.insertHistory(revIDBuffers,
                                                     rq.body,
                                                     rq.deletion,
                                                     rq.hasAttachments);
        if (commonAncestor < 0)
            error::_throw(error::InvalidParameter); // must be invalid revision IDs
        updateMeta();
        selectRevision(_versionedDoc[revidBuffer(rq.history[0])]);
        if (commonAncestor > 0 && rq.save)
            save(rq.maxRevTreeDepth);
        return commonAncestor;
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
            uint8_t revLen = (uint8_t)min((unsigned long)parentRevID.size, 255ul);
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
            uint8_t revLen = (uint8_t)min((unsigned long)parentRevID.size, 255ul);
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
        return revidBuffer(generation, digest, kDigestType);
    #else
        error::_throw(error::Unimplemented);
    #endif
    }


    bool TreeDocument::putNewRevision(const C4DocPutRequest &rq) {
        revidBuffer encodedRevID = generateDocRevID(rq.body, selectedRev.revID, rq.deletion);
        int httpStatus;
        auto newRev = _versionedDoc.insert(encodedRevID,
                                           rq.body,
                                           rq.deletion,
                                           rq.hasAttachments,
                                           _selectedRev,
                                           rq.allowConflict,
                                           httpStatus);
        if (newRev) {
            // Success:
            updateMeta();
            newRev = _versionedDoc.get(encodedRevID);
            selectRevision(newRev);
            if (rq.save)
                save(rq.maxRevTreeDepth);
            return true;
        } else if (httpStatus == 200) {
            // Revision already exists, so nothing was added. Not an error.
            selectRevision(encodedRevID.expanded(), true);
        } else if (httpStatus == 400) {
            error::_throw(error::InvalidParameter);
        } else if (httpStatus == 409) {
            error::_throw(error::Conflict);
        }
        return false;
    }

} // end namespace c4Internal


#pragma mark - DEPRECATED API:


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError) noexcept
{
    auto idoc = internal(doc);
    if (!idoc->mustUseVersioning(kC4RevisionTrees, outError))
        return -1;
    if (!idoc->mustBeInTransaction(outError))
        return false;
    try {
        ((TreeDocument*)idoc)->save(maxRevTreeDepth ? maxRevTreeDepth : kDefaultMaxRevTreeDepth);
        return true;
    } catchError(outError)
    return false;
}


C4SliceResult c4doc_generateRevID(C4Slice body, C4Slice parentRevID, bool deleted) noexcept {
    slice result = generateDocRevID(body, parentRevID, deleted).expanded().dontFree();
    return {result.buf, result.size};
}

unsigned c4rev_getGeneration(C4Slice revID) noexcept {
    try {
        return revidBuffer(revID).generation();
    }catchExceptions()
    return 0;
}

void c4doc_generateOldStyleRevID(bool generateOldStyle) noexcept {
    sGenerateOldStyleRevIDs = generateOldStyle;
}
