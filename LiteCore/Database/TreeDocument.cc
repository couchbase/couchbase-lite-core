//
// TreeDocument.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "Document.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "Record.hh"
#include "RawRevTree.hh"
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


        TreeDocument(const TreeDocument &other)
        :Document(other)
        ,_versionedDoc(other._versionedDoc)
        ,_selectedRev(nullptr)
        {
            if (other._selectedRev)
                _selectedRev = _versionedDoc[other._selectedRev->revID];
        }


        Document* copy() override {
            return new TreeDocument(*this);
        }


        void init() {
            docID = _docIDBuf = _versionedDoc.docID();
            flags = (C4DocumentFlags)_versionedDoc.flags();
            if (_versionedDoc.exists())
                flags = (C4DocumentFlags)(flags | kDocExists);

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

        bool exists() override {
            return _versionedDoc.exists();
        }

        bool revisionsLoaded() const noexcept override {
            return _versionedDoc.revsAvailable();
        }

        void loadRevisions() override {
            if (!_versionedDoc.revsAvailable()) {
                _versionedDoc.read();
                selectRevision(_versionedDoc.currentRevision());
            }
        }

        bool hasRevisionBody() noexcept override {
            if (!revisionsLoaded())
                Warn("c4doc_hasRevisionBody called on doc loaded without kC4IncludeBodies");
            return _selectedRev && _selectedRev->isBodyAvailable();
        }

        bool loadSelectedRevBody() override {
            loadRevisions();
            return selectedRev.body.buf != nullptr;
        }

        bool selectRevision(const Rev *rev) noexcept {   // doesn't throw
            _selectedRev = rev;
            _loadedBody = nullslice;
            if (rev) {
                _selectedRevIDBuf = rev->revID.expanded();
                selectedRev.revID = _selectedRevIDBuf;
                selectedRev.flags = (C4RevisionFlags)rev->flags;
                selectedRev.sequence = rev->sequence;
                selectedRev.body = rev->body();
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
                selectRevision(_selectedRev->parent);
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

        bool selectCommonAncestorRevision(slice revID1, slice revID2) override {
            const Rev *rev1 = _versionedDoc[revidBuffer(revID1)];
            const Rev *rev2 = _versionedDoc[revidBuffer(revID2)];
            if (!rev1 || !rev2)
                error::_throw(error::NotFound);
            while (rev1 != rev2) {
                int d = (int)rev1->revID.generation() - (int)rev2->revID.generation();
                if (d >= 0)
                    rev1 = rev1->parent;
                if (d <= 0)
                    rev2 = rev2->parent;
                if (!rev1 || !rev2)
                    return false;
            }
            selectRevision(rev1);
            return true;
        }

        alloc_slice remoteAncestorRevID(C4RemoteID remote) override {
            auto rev = _versionedDoc.latestRevisionOnRemote(remote);
            return rev ? rev->revID.expanded() : alloc_slice();
        }

        void setRemoteAncestorRevID(C4RemoteID remote) override {
            _versionedDoc.setLatestRevisionOnRemote(remote, _selectedRev);
        }

        void updateFlags() {
            flags = (C4DocumentFlags)_versionedDoc.flags() | kDocExists;
            initRevID();
        }

        bool removeSelectedRevBody() noexcept override {
            if (!_selectedRev)
                return false;
            _versionedDoc.removeBody(_selectedRev);
            return true;
        }

        bool save(unsigned maxRevTreeDepth) override {
            requireValidDocID();
            if (maxRevTreeDepth == 0)
                maxRevTreeDepth = _db->maxRevTreeDepth();
            _versionedDoc.prune(maxRevTreeDepth);
            switch (_versionedDoc.save(_db->transaction())) {
                case litecore::VersionedDocument::kConflict:
                    return false;
                case litecore::VersionedDocument::kNoNewSequence:
                    return true;
                case litecore::VersionedDocument::kNewSequence:
                    selectedRev.flags &= ~kRevNew;
                    if (_versionedDoc.sequence() > sequence) {
                        sequence = _versionedDoc.sequence();
                        if (selectedRev.sequence == 0)
                            selectedRev.sequence = sequence;
                        _db->saved(this);
                    }
                    return true;
            }
        }

        int32_t purgeRevision(C4Slice revID) override {
            int32_t total;
            if (revID.buf)
                total = _versionedDoc.purge(revidBuffer(revID));
            else
                total = _versionedDoc.purgeAll();
            if (total > 0) {
                _versionedDoc.updateMeta();
                updateFlags();
                if (_selectedRevIDBuf == revID)
                    selectRevision(_versionedDoc.currentRevision());
            }
            return total;
        }

        void resolveConflict(C4String winningRevID, C4String losingRevID, C4Slice mergedBody) override {
            // Validate the revIDs:
            auto winningRev = _versionedDoc[revidBuffer(winningRevID)];
            auto losingRev = _versionedDoc[revidBuffer(losingRevID)];
            if (!winningRev || !losingRev)
                error::_throw(error::NotFound);
            if (!winningRev->isLeaf() || !losingRev->isLeaf())
                error::_throw(error::Conflict);
            if (winningRev == losingRev)
                error::_throw(error::InvalidParameter);

            // Add a tombstone as a child of losingRev:
            selectRevision(losingRev);
            C4DocPutRequest rq = { };
            rq.revFlags = kRevDeleted;
            rq.history = &losingRevID;
            rq.historyCount = 1;
            rq.allowConflict = true;
            putNewRevision(rq);

            if (mergedBody.buf) {
                // Then add the new merged rev as a child of winningRev:
                selectRevision(winningRev);
                rq.revFlags = 0;
                rq.body = mergedBody;
                rq.history = &winningRevID;
                putNewRevision(rq);
            }
        }


#pragma mark - INSERTING REVISIONS


        int32_t putExistingRevision(const C4DocPutRequest &rq) override {
            Assert(rq.historyCount >= 1);
            int32_t commonAncestor = -1;
            loadRevisions();
            vector<revidBuffer> revIDBuffers(rq.historyCount);
            for (size_t i = 0; i < rq.historyCount; i++)
                revIDBuffers[i].parse(rq.history[i]);
            commonAncestor = _versionedDoc.insertHistory(revIDBuffers,
                                                         rq.body,
                                                         (Rev::Flags)rq.revFlags);
            if (commonAncestor < 0)
                error::_throw(error::BadRevisionID); // must be invalid revision IDs
            auto newRev = _versionedDoc[revidBuffer(rq.history[0])];
            DebugAssert(newRev);

            if (rq.remoteDBID)
                _versionedDoc.setLatestRevisionOnRemote(rq.remoteDBID, newRev);

            if (!saveNewRev(rq, newRev, (commonAncestor > 0 || rq.remoteDBID)))
                return -1;
            return commonAncestor;
        }


        bool putNewRevision(const C4DocPutRequest &rq) override {
            bool deletion = (rq.revFlags & kRevDeleted) != 0;
            revidBuffer encodedNewRevID = generateDocRevID(rq.body, selectedRev.revID, deletion);
            int httpStatus;
            auto newRev = _versionedDoc.insert(encodedNewRevID,
                                               rq.body,
                                               (Rev::Flags)rq.revFlags,
                                               _selectedRev,
                                               rq.allowConflict,
                                               httpStatus);
            if (newRev) {
                return saveNewRev(rq, newRev);
            } else if (httpStatus == 200) {
                // Revision already exists, so nothing was added. Not an error.
                selectRevision(toc4slice(encodedNewRevID.expanded()), true);
                return true;
            } else if (httpStatus == 400) {
                error::_throw(error::InvalidParameter);
            } else if (httpStatus == 409) {
                error::_throw(error::Conflict);
            } else {
                error::_throw(error::UnexpectedError);
            }
        }


        bool saveNewRev(const C4DocPutRequest &rq, const Rev *newRev NONNULL, bool reallySave =true) {
            selectRevision(newRev);
            if (rq.save && reallySave) {
                if (!save(rq.maxRevTreeDepth))
                    return false;
            } else {
                _versionedDoc.updateMeta();
            }
            updateFlags();
            return true;
        }


        static revidBuffer generateDocRevID(C4Slice body, C4Slice parentRevID, bool deleted) {
        #if SECURE_DIGEST_AVAILABLE
            uint8_t digestBuf[20];
            slice digest;
            // Get SHA-1 digest of (length-prefixed) parent rev ID, deletion flag, and revision body:
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


    private:
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

    DataFile::FleeceAccessor TreeDocumentFactory::fleeceAccessor() {
        return RawRevision::getCurrentRevBody;
    }

    alloc_slice TreeDocumentFactory::revIDFromVersion(slice version) {
        return revid(version).expanded();
    }

    bool TreeDocumentFactory::isFirstGenRevID(slice revID) {
        return revID.hasPrefix(slice("1-", 2));
    }

} // end namespace c4Internal


#pragma mark - REV-TREES-ONLY API:


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError) noexcept
{
    auto idoc = internal(doc);
    if (!idoc->mustUseVersioning(kC4RevisionTrees, outError))
        return false;
    if (!idoc->mustBeInTransaction(outError))
        return false;
    try {
        ((TreeDocument*)idoc)->save(maxRevTreeDepth ? maxRevTreeDepth : kDefaultMaxRevTreeDepth);
        return true;
    } catchError(outError)
    return false;
}


unsigned c4rev_getGeneration(C4Slice revID) noexcept {
    try {
        return revidBuffer(revID).generation();
    }catchExceptions()
    return 0;
}
