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

#include "Database.hh"
#include "Record.hh"
#include "RawRevTree.hh"
#include "VersionedDocument.hh"
#include "StringUtil.hh"
#include "SecureRandomize.hh"
#include "SecureDigest.hh"
#include "FleeceImpl.hh"
#include "varint.hh"
#include <ctime>
#include <algorithm>


namespace c4Internal {

    using namespace fleece;
    using namespace fleece::impl;

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
        :Document(database, doc.key()),
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
            _versionedDoc.owner = this;
            _versionedDoc.setPruneDepth(_db->maxRevTreeDepth());
            flags = (C4DocumentFlags)_versionedDoc.flags();
            if (_versionedDoc.exists())
                flags = (C4DocumentFlags)(flags | kDocExists);

            initRevID();
            selectCurrentRevision();
        }

        void initRevID() {
            setRevID(_versionedDoc.revID());
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
            } while (!rev->isLeaf() || rev->isClosed()
                                    || (!includeDeleted && rev->isDeleted()));
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

        Retained<Doc> fleeceDoc() override {
            slice body = selectedRev.body;
            if (!body)
                return nullptr;
            return new Doc(_versionedDoc.scopeFor(body), body, Doc::kTrusted);
        }

        bool save(unsigned maxRevTreeDepth =0) override {
            requireValidDocID();
            if (maxRevTreeDepth > 0)
                _versionedDoc.prune(maxRevTreeDepth);
            else
                _versionedDoc.prune();
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
                        _db->documentSaved(this);
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
                if (_selectedRevIDBuf == slice(revID))
                    selectRevision(_versionedDoc.currentRevision());
            }
            return total;
        }

        void resolveConflict(C4String winningRevID, C4String losingRevID,
                             C4Slice mergedBody, C4RevisionFlags mergedFlags) override
        {
            // Validate the revIDs:
            auto winningRev = _versionedDoc[revidBuffer(winningRevID)];
            auto losingRev = _versionedDoc[revidBuffer(losingRevID)];
            if (!winningRev || !losingRev)
                error::_throw(error::NotFound);
            if (!winningRev->isLeaf() || !losingRev->isLeaf())
                error::_throw(error::Conflict);
            if (winningRev == losingRev)
                error::_throw(error::InvalidParameter);

            _versionedDoc.markBranchAsConflict(winningRev, false);
            _versionedDoc.markBranchAsConflict(losingRev, false);

            // Add a tombstone as a child of losingRev:
            if (!losingRev->isClosed()) {
                selectRevision(losingRev);
                C4DocPutRequest rq = { };
                rq.revFlags = kRevDeleted | kRevClosed;
                rq.history = &losingRevID;
                rq.historyCount = 1;
                Assert(putNewRevision(rq));
            }

            if (mergedBody.buf) {
                // Then add the new merged rev as a child of winningRev:
                selectRevision(winningRev);
                C4DocPutRequest rq = { };
                rq.revFlags = mergedFlags & (kRevDeleted | kRevHasAttachments);
                rq.body = mergedBody;
                rq.history = &winningRevID;
                rq.historyCount = 1;
                Assert(putNewRevision(rq));
                LogTo(DBLog, "Resolved conflict, adding rev '%.*s' #%.*s",
                      SPLAT(docID), SPLAT(selectedRev.revID));
            }
        }


#pragma mark - INSERTING REVISIONS


        // Returns the body of the revision to be stored.
        alloc_slice requestBody(const C4DocPutRequest &rq, C4Error *outError) {
            alloc_slice body;
            if (rq.deltaCB == nullptr) {
                body = (rq.allocedBody.buf)? rq.allocedBody : alloc_slice(rq.body);
                if (!body)
                    body = alloc_slice{Dict::kEmpty, 2};
            } else {
                // Apply a delta via a callback:
                if (!rq.deltaSourceRevID.buf || !selectRevision(rq.deltaSourceRevID, true)) {
                    recordError(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                "Unknown source revision ID for delta", outError);
                } else if (!selectedRev.body.buf) {
                    recordError(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                "Unknown source revision body for delta", outError);
                } else {
                    slice delta = (rq.allocedBody.buf)? slice(rq.allocedBody) : slice(rq.body);
                    body = rq.deltaCB(rq.deltaCBContext, &selectedRev, delta, outError);
                }
            }

            // Now validate that the body is OK:
            if (body)
                database()->validateRevisionBody(body);
            return body;
        }


        int32_t putExistingRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            Assert(rq.historyCount >= 1);
            int32_t commonAncestor = -1;
            loadRevisions();
            vector<revidBuffer> revIDBuffers(rq.historyCount);
            for (size_t i = 0; i < rq.historyCount; i++)
                revIDBuffers[i].parse(rq.history[i]);

            alloc_slice body = requestBody(rq, outError);
            if (!body)
                return -1;

            if (rq.maxRevTreeDepth > 0)
                _versionedDoc.setPruneDepth(rq.maxRevTreeDepth);

            auto priorCurrentRev = _versionedDoc.currentRevision();
            int httpStatus;
            commonAncestor = _versionedDoc.insertHistory(revIDBuffers,
                                                         body,
                                                         (Rev::Flags)rq.revFlags,
                                                         rq.allowConflict,
                                                         (rq.remoteDBID != 0),
                                                         httpStatus);
            if (commonAncestor < 0) {
                if (outError) {
                    if (httpStatus == 409)
                        *outError = {LiteCoreDomain, kC4ErrorConflict};
                    else
                        *outError = c4error_make(LiteCoreDomain, kC4ErrorBadRevisionID,
                                                 "Bad revision history (non-sequential)"_sl);
                }
                return -1;
            }
            
            auto newRev = _versionedDoc[revidBuffer(rq.history[0])];
            DebugAssert(newRev);

            if (rq.remoteDBID) {
                auto oldRev = _versionedDoc.latestRevisionOnRemote(rq.remoteDBID);
                if (oldRev && !oldRev->isAncestorOf(newRev)) {
                    // Server has "switched branches": its current revision is now on a different
                    // branch than it used to be, either due to revs added to this branch, or
                    // deletion of the old branch. In either case this is not a conflict.
                    Assert(newRev->isConflict());
                    const char *effect;
                    if (oldRev->isConflict()) {
                        _versionedDoc.purge(oldRev->revID);
                        effect = "purging old branch";
                    } else if (oldRev == priorCurrentRev) {
                        _versionedDoc.markBranchAsConflict(newRev, false);
                        _versionedDoc.purge(oldRev->revID);
                        effect = "making new branch main & purging old";
                        Assert(_versionedDoc.currentRevision() == newRev);
                    } else {
                        effect = "doing nothing";
                    }
                    LogTo(DBLog, "c4doc_put detected server-side branch-switch: \"%.*s\" %.*s to %.*s; %s",
                          SPLAT(docID), SPLAT(oldRev->revID.expanded()),
                          SPLAT(newRev->revID.expanded()), effect);
                }
                _versionedDoc.setLatestRevisionOnRemote(rq.remoteDBID, newRev);
            }

            if (!saveNewRev(rq, newRev, (commonAncestor > 0 || rq.remoteDBID))) {
                if (outError)
                    *outError = {LiteCoreDomain, kC4ErrorConflict};
                return -1;
            }
            return commonAncestor;
        }


        bool putNewRevision(const C4DocPutRequest &rq) override {
            if (rq.remoteDBID != 0)
                error::_throw(error::InvalidParameter, "remoteDBID cannot be used when existing=false");
            bool deletion = (rq.revFlags & kRevDeleted) != 0;

            if (rq.maxRevTreeDepth > 0)
                _versionedDoc.setPruneDepth(rq.maxRevTreeDepth);

            C4Error err;
            alloc_slice body = requestBody(rq, &err);
            if (!body)
                error::_throw((error::Domain)err.domain, err.code); //FIX: Ick.

            revidBuffer encodedNewRevID = generateDocRevID(body, selectedRev.revID, deletion);

            int httpStatus;
            auto newRev = _versionedDoc.insert(encodedNewRevID,
                                               body,
                                               (Rev::Flags)rq.revFlags,
                                               _selectedRev,
                                               rq.allowConflict,
                                               false,
                                               httpStatus);
            if (newRev) {
                return saveNewRev(rq, newRev);
            } else if (httpStatus == 200) {
                // Revision already exists, so nothing was added. Not an error.
                selectRevision(encodedNewRevID.expanded(), true);
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
                if (!save())
                    return false;
                if (_db->dataFile()->willLog(LogLevel::Verbose)) {
                    _db->dataFile()->_logVerbose( "%-s '%.*s' rev #%s as seq %" PRIu64,
                        ((rq.revFlags & kRevDeleted) ? "Deleted" : "Saved"),
                        SPLAT(rq.docID), string(newRev->revID).c_str(), _versionedDoc.sequence());
                }
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


    Retained<Document> TreeDocumentFactory::newDocumentInstance(C4Slice docID) {
        return new TreeDocument(database(), docID);
    }

    Retained<Document> TreeDocumentFactory::newDocumentInstance(const Record &doc) {
        return new TreeDocument(database(), doc);
    }

    slice TreeDocumentFactory::fleeceAccessor(slice docBody) {
        return RawRevision::getCurrentRevBody(docBody);
    }

    alloc_slice TreeDocumentFactory::revIDFromVersion(slice version) {
        return revid(version).expanded();
    }

    bool TreeDocumentFactory::isFirstGenRevID(slice revID) {
        return revID.hasPrefix(slice("1-", 2));
    }

    Document* TreeDocumentFactory::treeDocumentContaining(const Value *value) {
        VersionedDocument *vdoc = VersionedDocument::containing(value);
        return vdoc ? (TreeDocument*)vdoc->owner : nullptr;
    }

} // end namespace c4Internal


#pragma mark - REV-TREES-ONLY API:


bool c4doc_save(C4Document *doc,
                uint32_t maxRevTreeDepth,
                C4Error *outError) noexcept
{
    auto idoc = asInternal(doc);
    if (!idoc->mustUseVersioning(kC4RevisionTrees, outError))
        return false;
    if (!idoc->mustBeInTransaction(outError))
        return false;
    try {
        if (maxRevTreeDepth == 0)
            maxRevTreeDepth = asInternal(doc)->database()->maxRevTreeDepth();
        
        if (!((TreeDocument*)idoc)->save(maxRevTreeDepth)) {
            if (outError)
                *outError = {LiteCoreDomain, kC4ErrorConflict};
            return false;
        }
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
