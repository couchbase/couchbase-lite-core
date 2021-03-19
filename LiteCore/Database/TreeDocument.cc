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

#include "TreeDocument.hh"
#include "c4Private.h"

#include "Database.hh"
#include "Record.hh"
#include "RawRevTree.hh"
#include "RevTreeRecord.hh"
#include "Delimiter.hh"
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
    using namespace std;

    class TreeDocument : public Document {
    public:
        TreeDocument(Database* database, C4Slice docID, ContentOption content)
        :Document(database, docID),
         _revTree(database->defaultKeyStore(), docID, content),
         _selectedRev(nullptr)
        {
            init();
        }


        TreeDocument(Database *database, const Record &doc)
        :Document(database, doc.key()),
         _revTree(database->defaultKeyStore(), doc),
         _selectedRev(nullptr)
        {
            init();
        }


        TreeDocument(const TreeDocument &other)
        :Document(other)
        ,_revTree(other._revTree)
        ,_selectedRev(nullptr)
        {
            if (other._selectedRev)
                _selectedRev = _revTree[other._selectedRev->revID];
        }


        void init() {
            _revTree.owner = this;
            _revTree.setPruneDepth(_db->maxRevTreeDepth());
            flags = (C4DocumentFlags)_revTree.flags();
            if (_revTree.exists())
                flags = (C4DocumentFlags)(flags | kDocExists);

            initRevID();
            selectCurrentRevision();
        }

        void initRevID() {
            setRevID(_revTree.revID());
            sequence = _revTree.sequence();
        }

        bool exists() noexcept override {
            return _revTree.exists();
        }

        bool revisionsLoaded() const noexcept override {
            return _revTree.revsAvailable();
        }

        void requireRevisions() const {
            if (!_revTree.revsAvailable())
                error::_throw(error::UnsupportedOperation,
                        "This function is not legal on a C4Document loaded without kDocGetAll");
        }

        // This method can throw exceptions, so should not be called from 'noexcept' overrides!
        // Such methods should call requireRevisions instead.
        void loadRevisions() override {
            if (!_revTree.revsAvailable()) {
                LogTo(DBLog, "Need to read rev-tree of doc '%.*s'", SPLAT(docID));
                _revTree.read(kEntireBody);
                selectRevision(_revTree.currentRevision());
            }
        }

        bool hasRevisionBody() noexcept override {
            if (_revTree.revsAvailable())
                return _selectedRev && _selectedRev->isBodyAvailable();
            else
                return _revTree.currentRevAvailable();
        }

        bool loadSelectedRevBody() override {
            if (!_selectedRev && _revTree.currentRevAvailable())
                return true;            // only the current rev is available, so return true
            loadRevisions();
            return _selectedRev &&_selectedRev->body();
        }

        virtual slice getSelectedRevBody() noexcept override {
            if (_selectedRev)
                return _selectedRev->body();
            else if (_revTree.currentRevAvailable())
                return _revTree.currentRevBody();
            else
                return nullslice;
        }


        alloc_slice getSelectedRevHistory(unsigned maxRevs,
                                          const C4String backToRevs[],
                                          unsigned backToRevsCount) override
        {
            auto selRev = _selectedRev;
            int revsWritten = 0;
            stringstream historyStream;
            string::size_type lastPos = 0;

            if (maxRevs == 0)
                maxRevs = UINT_MAX;

            auto append = [&](slice revID) {
                lastPos = (string::size_type)historyStream.tellp();
                if (revsWritten++ > 0)
                    historyStream << ',';
                historyStream.write((const char*)revID.buf, revID.size);
            };

            auto hasRemoteAncestor = [&](C4String revID) {
                for (unsigned i = 0; i < backToRevsCount; ++i)
                    if (slice(backToRevs[i]) == slice(revID))
                        return true;
                return false;
            };

            auto removeLast = [&]() {
                string buf = historyStream.str();
                buf.resize(lastPos);
                historyStream.str(buf);
                historyStream.seekp(lastPos);
                --revsWritten;
            };

            // Go back through history, starting with the desired rev's parent, until we either reach
            // a rev known to the peer or we run out of history. Do not write more than `maxHistory`
            // revisions, but always write the rev known to the peer if there is one.
            // There may be gaps in the history (non-consecutive generations) if revs have been pruned.
            // If sending these, make up random revIDs for them since they don't matter.
            unsigned lastGen = c4rev_getGeneration(selectedRev.revID) + 1;
            do {
                slice revID = selectedRev.revID;
                unsigned gen = c4rev_getGeneration(revID);
                while (gen < --lastGen && revsWritten < maxRevs) {
                    // We don't have this revision (the history got deeper than the local db's
                    // maxRevTreeDepth), so make up a random revID. The server probably won't care.
                    append(slice(format("%u-faded000%.08x%.08x",
                                        lastGen, RandomNumber(), RandomNumber())));
                }
                lastGen = gen;

                if (hasRemoteAncestor(revID)) {
                    // Always write the common ancestor, making room if necessary:
                    if (revsWritten == maxRevs)
                        removeLast();
                    append(revID);
                    break;
                } else {
                    // Write a regular revision if there's room:
                    if (revsWritten < maxRevs) {
                        append(revID);
                        if (backToRevsCount == 0 && revsWritten == maxRevs)
                            break;
                    }
                }
            } while (selectParentRevision());
            selectRevision(selRev);
            return alloc_slice(historyStream.str());
        }


        bool selectRevision(const Rev *rev) noexcept {   // doesn't throw
            _selectedRev = rev;
            if (rev) {
                _selectedRevIDBuf = rev->revID.expanded();
                selectedRev.revID = _selectedRevIDBuf;
                selectedRev.flags = (C4RevisionFlags)rev->flags;
                selectedRev.sequence = rev->sequence;
                return true;
            } else {
                clearSelectedRevision();
                return false;
            }
        }

        bool selectRevision(C4Slice revID, bool withBody) override {
            if (revID.buf) {
                loadRevisions();
                const Rev *rev = _revTree[revidBuffer(revID)];
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
            if (_revTree.revsAvailable()) {
                selectRevision(_revTree.currentRevision());
                return true;
            } else {
                _selectedRev = nullptr;
                Document::selectCurrentRevision();
                return false;
            }
        }

        bool selectParentRevision() noexcept override {
            requireRevisions();
            if (_selectedRev)
                selectRevision(_selectedRev->parent);
            return _selectedRev != nullptr;
        }

        bool selectNextRevision() noexcept override {    // does not throw
            requireRevisions();
            if (_selectedRev)
                selectRevision(_selectedRev->next());
            return _selectedRev != nullptr;
        }

        bool selectNextLeafRevision(bool includeDeleted) noexcept override {
            requireRevisions();
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
            requireRevisions();
            const Rev *rev1 = _revTree[revidBuffer(revID1)];
            const Rev *rev2 = _revTree[revidBuffer(revID2)];
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
            loadRevisions();
            auto rev = _revTree.latestRevisionOnRemote(remote);
            return rev ? rev->revID.expanded() : alloc_slice();
        }

        void setRemoteAncestorRevID(C4RemoteID remote, C4String revID) override {
            loadRevisions();
            const Rev *rev = _revTree[revidBuffer(revID)];
            if (!rev)
                error::_throw(error::NotFound);
            _revTree.setLatestRevisionOnRemote(remote, rev);
        }

        void updateFlags() {
            flags = (C4DocumentFlags)_revTree.flags() | kDocExists;
            initRevID();
        }

        bool removeSelectedRevBody() noexcept override {
            if (!_selectedRev)
                return false;
            _revTree.removeBody(_selectedRev);
            return true;
        }

        bool save(unsigned maxRevTreeDepth =0) override {
            requireValidDocID();
            if (maxRevTreeDepth > 0)
                _revTree.prune(maxRevTreeDepth);
            else
                _revTree.prune();
            switch (_revTree.save(_db->transaction())) {
                case litecore::RevTreeRecord::kConflict:
                    return false;
                case litecore::RevTreeRecord::kNoNewSequence:
                    return true;
                case litecore::RevTreeRecord::kNewSequence:
                    selectedRev.flags &= ~kRevNew;
                    if (_revTree.sequence() > sequence) {
                        sequence = _revTree.sequence();
                        if (selectedRev.sequence == 0)
                            selectedRev.sequence = sequence;
                        _db->documentSaved(this);
                    }
                    return true;
                default:
                    Assert(false, "Invalid save result received");
            }
        }

        int32_t purgeRevision(C4Slice revID) override {
            loadRevisions();
            int32_t total;
            if (revID.buf)
                total = _revTree.purge(revidBuffer(revID));
            else
                total = _revTree.purgeAll();
            if (total > 0) {
                _revTree.updateMeta();
                updateFlags();
                if (_selectedRevIDBuf == slice(revID))
                    selectRevision(_revTree.currentRevision());
            }
            return total;
        }

        void resolveConflict(C4String winningRevID, C4String losingRevID,
                             C4Slice mergedBody, C4RevisionFlags mergedFlags,
                             bool pruneLosingBranch =true) override
        {
            loadRevisions();
            // Validate the revIDs:
            auto winningRev = _revTree[revidBuffer(winningRevID)];
            auto losingRev = _revTree[revidBuffer(losingRevID)];
            if (!winningRev || !losingRev)
                error::_throw(error::NotFound);
            if (!winningRev->isLeaf() || !losingRev->isLeaf())
                error::_throw(error::Conflict);
            if (winningRev == losingRev)
                error::_throw(error::InvalidParameter);

            _revTree.markBranchAsNotConflict(winningRev, true);
            _revTree.markBranchAsNotConflict(losingRev, false);

            // Deal with losingRev:
            if (pruneLosingBranch) {
                // Purge its branch entirely
                purgeRevision(losingRevID);
            } else if (!losingRev->isClosed()) {
                // or just put a tombstone on top of it
                selectRevision(losingRev);
                C4DocPutRequest rq = { };
                rq.revFlags = kRevDeleted | kRevClosed;
                rq.history = &losingRevID;
                rq.historyCount = 1;
                Assert(putNewRevision(rq, nullptr));
            }

            if (mergedBody.buf) {
                // Then add the new merged rev as a child of winningRev:
                alloc_slice emptyDictBody;
                if (mergedBody.size == 0) {
                    // An empty body isn't legal, so replace it with an encoded empty Dict:
                    Encoder enc;
                    enc.beginDictionary();
                    enc.endDictionary();
                    emptyDictBody = enc.finish();
                    mergedBody = emptyDictBody;
                }

                selectRevision(winningRev);
                C4DocPutRequest rq = { };
                rq.revFlags = mergedFlags & (kRevDeleted | kRevHasAttachments);
                rq.body = mergedBody;
                rq.history = &winningRevID;
                rq.historyCount = 1;
                Assert(putNewRevision(rq, nullptr));
                LogTo(DBLog, "Resolved conflict, adding rev '%.*s' #%.*s",
                      SPLAT(docID), SPLAT(selectedRev.revID));
            } else if(winningRev->sequence == sequence) {
                // CBL-1089
                // In this case the winning revision both had no body, meaning that it
                // already existed in the database previous with the conflict flag,
                // and its sequence matches the latest sequence of the document.
                // This means that it has not been entered into the sequence tracker
                // yet, because the documentSaved method will not consider conflicts,
                // but it needs to be now that it is resolved.  It can't go in here
                // because the sequence may be invalid by this point so instead reset
                // the sequence to 0 so that the required follow-up call to save() will
                // generate a new one for it and then _that_ will go into the sequence
                // tracker.
                _revTree.resetConflictSequence(winningRev);
                selectRevision(winningRev);
            }
        }


#pragma mark - INSERTING REVISIONS


        // Returns the body of the revision to be stored.
        alloc_slice requestBody(const C4DocPutRequest &rq, C4Error *outError) {
            alloc_slice body;
            if (rq.deltaCB == nullptr) {
                body = (rq.allocedBody.buf)? rq.allocedBody : alloc_slice(rq.body);
                if (!body)
                    body = Encoder::kPreEncodedEmptyDict;
            } else {
                // Apply a delta via a callback:
                if (!rq.deltaSourceRevID.buf || !selectRevision(rq.deltaSourceRevID, true)) {
                    if (outError)
                        *outError = c4error_printf(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                               "Missing source revision '%.*s' for delta",
                                               SPLAT(rq.deltaSourceRevID));
                } else if (!getSelectedRevBody()) {
                    if (outError)
                        *outError = c4error_printf(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                               "Missing body of source revision '%.*s' for delta",
                                               SPLAT(rq.deltaSourceRevID));
                } else {
                    slice delta = (rq.allocedBody.buf)? slice(rq.allocedBody) : slice(rq.body);
                    body = rq.deltaCB(rq.deltaCBContext, this, delta, outError);
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
            if (!body) {
                if (outError && outError->code == kC4ErrorDeltaBaseUnknown
                             && outError->domain == LiteCoreDomain) {
                    // A missing delta base might just be a side effect of a conflict:
                    if (!rq.allowConflict
                        && _revTree.findCommonAncestor(revIDBuffers, rq.allowConflict).second
                                == -409) {
                        *outError = c4error_make(LiteCoreDomain, kC4ErrorConflict, nullslice);
                    } else {
                        alloc_slice cur = _revTree.currentRevision()->revID.expanded();
                        Warn("Missing base rev for delta! Inserting rev %.*s, delta base is %.*s, "
                             "doc current rev is %.*s",
                             SPLAT(rq.history[0]), SPLAT(rq.deltaSourceRevID), SPLAT(cur));
                    }
                }
                return -1;
            }

            if (rq.maxRevTreeDepth > 0)
                _revTree.setPruneDepth(rq.maxRevTreeDepth);

            auto priorCurrentRev = _revTree.currentRevision();
            commonAncestor = _revTree.insertHistory(revIDBuffers,
                                                         body,
                                                         (Rev::Flags)rq.revFlags,
                                                         rq.allowConflict,
                                                         (rq.remoteDBID != 0));
            if (commonAncestor < 0) {
                if (outError) {
                    alloc_slice current = _revTree.revID().expanded();
                    LogWarn(DBLog,
                           "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> %d",
                           SPLAT(docID), SPLAT(rq.history[0]), SPLAT(current), -commonAncestor);
                    if (commonAncestor == -409)
                        *outError = {LiteCoreDomain, kC4ErrorConflict};
                    else
                        *outError = c4error_make(LiteCoreDomain, kC4ErrorBadRevisionID,
                                                 "Bad revision history (non-sequential)"_sl);
                }
                return -1;
            }
            
            auto newRev = _revTree[revidBuffer(rq.history[0])];
            DebugAssert(newRev);

            if (rq.remoteDBID) {
                auto oldRev = _revTree.latestRevisionOnRemote(rq.remoteDBID);
                if (oldRev && !oldRev->isAncestorOf(newRev)) {
                    if(newRev->isAncestorOf(oldRev)) {
                        // CBL-578: Sometimes due to the parallel nature of the rev responses, older
                        // revs come in after newer ones.  In this case, we will just ignore the older
                        // rev that has come through
                        LogTo(DBLog, "Document \"%.*s\" received older revision %.*s after %.*s, ignoring...",
                              SPLAT(docID), SPLAT(newRev->revID.expanded()), SPLAT(oldRev->revID.expanded()));
                        return (int32_t)oldRev->revID.generation();
                    }
                    
                    // Server has "switched branches": its current revision is now on a different
                    // branch than it used to be, either due to revs added to this branch, or
                    // deletion of the old branch. In either case this is not a conflict.
                    Assert(newRev->isConflict());
                    const char *effect;
                    if (oldRev->isConflict()) {
                        _revTree.purge(oldRev->revID);
                        effect = "purging old branch";
                    } else if (oldRev == priorCurrentRev) {
                        _revTree.markBranchAsNotConflict(newRev, true);
                        _revTree.purge(oldRev->revID);
                        effect = "making new branch main & purging old";
                        Assert(_revTree.currentRevision() == newRev);
                    } else {
                        effect = "doing nothing";
                    }
                    LogTo(DBLog, "c4doc_put detected server-side branch-switch: \"%.*s\" %.*s to %.*s; %s",
                          SPLAT(docID), SPLAT(oldRev->revID.expanded()),
                          SPLAT(newRev->revID.expanded()), effect);
                }
                _revTree.setLatestRevisionOnRemote(rq.remoteDBID, newRev);
            }

            if (!saveNewRev(rq, newRev, (commonAncestor > 0 || rq.remoteDBID))) {
                if (outError)
                    *outError = {LiteCoreDomain, kC4ErrorConflict};
                return -1;
            }
            return commonAncestor;
        }


        bool putNewRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            bool deletion = (rq.revFlags & kRevDeleted) != 0;

            if (rq.maxRevTreeDepth > 0)
                _revTree.setPruneDepth(rq.maxRevTreeDepth);

            alloc_slice body = requestBody(rq, outError);
            if (!body)
                return false;

            revidBuffer encodedNewRevID = generateDocRevID(body, selectedRev.revID, deletion);

            C4ErrorCode errorCode = {};
            int httpStatus;
            auto newRev = _revTree.insert(encodedNewRevID,
                                               body,
                                               (Rev::Flags)rq.revFlags,
                                               _selectedRev,
                                               rq.allowConflict,
                                               false,
                                               httpStatus);
            if (newRev) {
                if (!saveNewRev(rq, newRev))
                    errorCode = kC4ErrorConflict;
            } else if (httpStatus == 200) {
                // Revision already exists, so nothing was added. Not an error.
                selectRevision(encodedNewRevID.expanded(), true);
            } else if (httpStatus == 400) {
                errorCode = kC4ErrorInvalidParameter;
            } else if (httpStatus == 409) {
                errorCode = kC4ErrorConflict;
            } else {
                errorCode = kC4ErrorUnexpectedError;
            }

            if (errorCode) {
                c4error_return(LiteCoreDomain, errorCode, nullslice, outError);
                return false;
            }
            return true;
        }


        bool saveNewRev(const C4DocPutRequest &rq, const Rev *newRev NONNULL, bool reallySave =true) {
            selectRevision(newRev);
            if (rq.save && reallySave) {
                if (!save())
                    return false;
                if (_db->dataFile()->willLog(LogLevel::Verbose)) {
                    alloc_slice revID = newRev->revID.expanded();
                    _db->dataFile()->_logVerbose( "%-s '%.*s' rev #%.*s as seq %" PRIu64,
                        ((rq.revFlags & kRevDeleted) ? "Deleted" : "Saved"),
                        SPLAT(rq.docID), SPLAT(revID), _revTree.sequence());
                }
            } else {
                _revTree.updateMeta();
            }
            updateFlags();
            return true;
        }


        static revidBuffer generateDocRevID(C4Slice body, C4Slice parentRevID, bool deleted) {
            // Get SHA-1 digest of (length-prefixed) parent rev ID, deletion flag, and revision body:
            uint8_t revLen = (uint8_t)min((unsigned long)parentRevID.size, 255ul);
            uint8_t delByte = deleted;
            SHA1 digest = (SHA1Builder() << revLen << slice(parentRevID.buf, revLen)
                                         << delByte << body)
                           .finish();
            // Derive new rev's generation #:
            unsigned generation = 1;
            if (parentRevID.buf) {
                revidBuffer parentID(parentRevID);
                generation = parentID.generation() + 1;
            }
            return revidBuffer(generation, slice(digest));
        }


    private:
        RevTreeRecord _revTree;
        const Rev *_selectedRev;
    };


#pragma mark - FACTORY:


    Retained<Document> TreeDocumentFactory::newDocumentInstance(C4Slice docID, ContentOption c) {
        return new TreeDocument(database(), docID, c);
    }

    Retained<Document> TreeDocumentFactory::newDocumentInstance(const Record &rec) {
        return new TreeDocument(database(), rec);
    }

    bool TreeDocumentFactory::isFirstGenRevID(slice revID) const {
        return revID.hasPrefix(slice("1-", 2));
    }

    Document* TreeDocumentFactory::documentContaining(FLValue value) {
        RevTreeRecord *vdoc = RevTreeRecord::containing((const Value*)value);
        return vdoc ? (TreeDocument*)vdoc->owner : nullptr;
    }

    vector<alloc_slice> TreeDocumentFactory::findAncestors(const vector<slice> &docIDs,
                                                           const vector<slice> &revIDs,
                                                           unsigned maxAncestors,
                                                           bool mustHaveBodies,
                                                           C4RemoteID remoteDBID)
    {
        // Map docID->revID for faster lookup in the callback:
        unordered_map<slice,slice> revMap(docIDs.size());
        for (ssize_t i = docIDs.size() - 1; i >= 0; --i)
            revMap[docIDs[i]] = revIDs[i];
        stringstream result;

        auto callback = [&](const RecordLite &rec) -> alloc_slice {
            // --- This callback runs inside the SQLite query ---
            // --- It will be called once for each docID in the vector ---
            // Convert revID to encoded binary form:
            revidBuffer revID;
            revID.parse(revMap[rec.key]);
            auto revGeneration = revID.generation();
            C4FindDocAncestorsResultFlags status = {};
            RevTree tree(rec.body, rec.extra, 0);
            auto current = tree.currentRevision();

            // Does it exist in the doc?
            if (const Rev *rev = tree[revID]) {
                if (rev->isBodyAvailable())
                    status |= kRevsHaveLocal;
                if (remoteDBID && rev == tree.latestRevisionOnRemote(remoteDBID))
                    status |= kRevsAtThisRemote;
                if (current != rev) {
                    if (rev->isAncestorOf(current))
                        status |= kRevsLocalIsNewer;
                    else
                        status |= kRevsConflict;
                }
            } else {
                if (current->revID.generation() < revGeneration)
                    status |= kRevsLocalIsOlder;
                else
                    status |= kRevsConflict;
            }

            char statusChar = '0' + char(status);
            if (!(status & kRevsLocalIsOlder)) {
                return alloc_slice(&statusChar, 1);
            }

            // Find revs that could be ancestors of it and write them as a JSON array:
            result.str("");
            result << statusChar << '[';
            char expandedBuf[100];
            delimiter delim(",");
            for (auto rev : tree.allRevisions()) {
                if (rev->revID.generation() < revGeneration
                            && !(mustHaveBodies && !rev->isBodyAvailable())) {
                    slice expanded(expandedBuf, sizeof(expandedBuf));
                    if (rev->revID.expandInto(expanded)) {
                        result << delim << '"' << expanded << '"';
                        if (delim.count() >= maxAncestors)
                            break;
                    }
                }
            }
            result << ']';
            return alloc_slice(result.str());
        };
        return database()->dataFile()->defaultKeyStore().withDocBodies(docIDs, callback);
    }


} // end namespace c4Internal
