//
// VectorDocument.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "VectorDocument.hh"
#include "VectorRecord.hh"
#include "VersionVector.hh"
#include "c4Document+Fleece.h"
#include "c4Private.h"
#include "Delimiter.hh"
#include "StringUtil.hh"
#include <set>

namespace c4Internal {
    using namespace std;
    using namespace fleece;


    class VectorDocument final : public Document {
    public:
        VectorDocument(Database* database, C4Slice docID, ContentOption whichContent)
        :Document(database, docID)
        ,_doc(database->defaultKeyStore(), Versioning::Vectors, docID, whichContent)
        {
            _initialize();
        }


        VectorDocument(Database *database, const Record &doc)
        :Document(database, doc.key())
        ,_doc(database->defaultKeyStore(), Versioning::Vectors, doc)
        {
            _initialize();
        }


        ~VectorDocument() {
            _doc.owner = nullptr;
        }


        void _initialize() {
            _doc.owner = this;
            _doc.setEncoder(_db->sharedFLEncoder());
            _updateDocFields();
            _selectRemote(RemoteID::Local);
        }


        void _updateDocFields() {
            _revIDBuf = _expandRevID(_doc.revID());
            revID = _revIDBuf;

            flags = C4DocumentFlags(_doc.flags());
            if (_doc.exists())
                flags |= kDocExists;
            sequence = _doc.sequence();
        }


        peerID myPeerID() {
            return peerID{database()->myPeerID()};
        }


        alloc_slice _expandRevID(revid rev, peerID myID =kMePeerID) {
            if (!rev)
                return nullslice;
            return rev.asVersion().asASCII(myID);
        }


        revidBuffer _parseRevID(slice revID) {
            if (revID) {
                if (revidBuffer binaryID(revID); binaryID.isVersion()) {
                    // If it's a version in global form, convert it to local form:
                    if (auto vers = binaryID.asVersion(); vers.author() == myPeerID())
                    binaryID = Version(revID, myPeerID());
                    return binaryID;
                }
            }
            error::_throw(error::BadRevisionID, "Not a version string: '%.*s'", SPLAT(revID));
        }


#pragma mark - SELECTING REVISIONS:


        optional<pair<RemoteID, Revision>> _findRemote(slice revID) {
            RemoteID remote = RemoteID::Local;
            if (revID.findByte(',')) {
                // It's a version vector; look for an exact match:
                VersionVector vers = VersionVector::fromASCII(revID, myPeerID());
                alloc_slice binary = vers.asBinary();
                while (auto rev = _doc.loadRemoteRevision(remote)) {
                    if (rev->revID == binary)
                        return {{remote, *rev}};
                    remote = _doc.loadNextRemoteID(remote);
                }
            } else {
                // It's a single version, so find a vector that starts with it:
                Version vers = _parseRevID(revID).asVersion();
                while (auto rev = _doc.loadRemoteRevision(remote)) {
                    if (rev->revID && rev->version() == vers)
                        return {{remote, *rev}};
                    remote = _doc.loadNextRemoteID(remote);
                }
            }
            return nullopt;
        }


        // intentionally does not load other revisions ... throws if they're not in memory.
        // Calling code should be fixed to load the doc with all revisions using c4db_getDoc2.
        bool _selectRemote(RemoteID remote) {
            if (auto rev = _doc.remoteRevision(remote); rev && rev->revID) {
                return _selectRemote(remote, *rev);
            } else {
                _remoteID = nullopt;
                clearSelectedRevision();
                return false;
            }
        }


        bool _selectRemote(RemoteID remote, Revision &rev) {
            _remoteID = remote;
            _selectedRevIDBuf = _expandRevID(rev.revID);
            selectedRev.revID = _selectedRevIDBuf;
            selectedRev.sequence = _doc.sequence(); // VectorRecord doesn't have per-rev sequence

            selectedRev.flags = 0;
            if (remote == RemoteID::Local)  selectedRev.flags |= kRevLeaf;
            if (rev.isDeleted())            selectedRev.flags |= kRevDeleted;
            if (rev.hasAttachments())       selectedRev.flags |= kRevHasAttachments;
            if (rev.isConflicted())         selectedRev.flags |= kRevIsConflict | kRevLeaf;
            return true;
        }


        bool selectRevision(C4Slice revID, bool withBody) override {
            if (auto r = _findRemote(revID); r) {
                return _selectRemote(r->first, r->second);
            } else {
                _remoteID = nullopt;
                clearSelectedRevision();
                return false;
            }
        }


        optional<Revision> _selectedRevision() {
            return _remoteID ? _doc.remoteRevision(*_remoteID) : nullopt;
        }


        bool selectCurrentRevision() noexcept override {
            return _selectRemote(RemoteID::Local);
        }


        bool selectNextRevision() override {
            return _remoteID && _selectRemote(_doc.nextRemoteID(*_remoteID));
        }


        bool selectNextLeafRevision(bool includeDeleted) override {
            while (selectNextRevision()) {
                if (selectedRev.flags & kRevLeaf)
                    return true;
            }
            return false;
        }

        
#pragma mark - ACCESSORS:


        slice getSelectedRevBody() noexcept override {
            if (auto rev = _selectedRevision()) {
                // Current revision, or remote with the same version:
                if (rev->revID == _doc.revID())
                    return _doc.currentRevisionData();

                // Else the properties have to be re-encoded to a slice:
                if (rev->properties) {
                    SharedEncoder enc(_db->sharedFLEncoder());
                    enc << rev->properties;
                    _latestBody = enc.finishDoc();
                    return _latestBody.data();;
                }
            }
            return nullslice;
        }


        FLDict getSelectedRevRoot() noexcept override {
            auto rev = _selectedRevision();
            return rev ? rev->properties : nullptr;
        }


        alloc_slice getSelectedRevIDGlobalForm() override {
            if (auto rev = _selectedRevision(); rev)
                return rev->versionVector().asASCII(myPeerID());
            else
                return nullslice;
        }


        alloc_slice getSelectedRevHistory(unsigned maxRevs,
                                          const C4String backToRevs[],
                                          unsigned backToRevsCount) override
        {
            if (auto rev = _selectedRevision(); rev) {
                VersionVector vers = rev->versionVector();
                if (maxRevs > 0 && vers.count() > maxRevs)
                    vers.limitCount(maxRevs);
                // Easter egg: if maxRevs is 0, don't replace '*' with my peer ID [tests use this]
                return vers.asASCII(maxRevs ? myPeerID() : kMePeerID);
            } else {
                return nullslice;
            }
        }


        alloc_slice remoteAncestorRevID(C4RemoteID remote) override {
            if (auto rev = _doc.loadRemoteRevision(RemoteID(remote)))
                return rev->revID.expanded();
            return nullptr;
        }


        void setRemoteAncestorRevID(C4RemoteID remote, C4String revID) override {
            Assert(RemoteID(remote) != RemoteID::Local);
            Revision revision;
            revidBuffer vers(revID);
            if (auto r = _findRemote(revID); r)
                revision = r->second;
            else
                revision.revID = vers;
            _doc.setRemoteRevision(RemoteID(remote), revision);
        }


#pragma mark - EXISTENCE / LOADING:


        bool exists() override {
            return _doc.exists();
        }


        bool loadRevisions() override {
            return _doc.contentAvailable() >= kEntireBody || _doc.loadData(kEntireBody);
        }


        bool revisionsLoaded() const noexcept override {
            return _doc.contentAvailable() >= kEntireBody;
        }


        bool hasRevisionBody() noexcept override {
            return _doc.exists() && _remoteID;
        }


        bool loadSelectedRevBody() override {
            if (!_remoteID)
                return false;
            auto which = (*_remoteID == RemoteID::Local) ? kCurrentRevOnly : kEntireBody;
            return _doc.loadData(which);
        }


#pragma mark - UPDATING:


        VersionVector _currentVersionVector() {
            auto curRevID = _doc.revID();
            return curRevID ? curRevID.asVersionVector() : VersionVector();
        }


        static DocumentFlags convertNewRevisionFlags(C4RevisionFlags revFlags) {
            DocumentFlags docFlags = {};
            if (revFlags & kRevDeleted)        docFlags |= DocumentFlags::kDeleted;
            if (revFlags & kRevHasAttachments) docFlags |= DocumentFlags::kHasAttachments;
            return docFlags;
        }

        
        fleece::Doc _newProperties(const C4DocPutRequest &rq, C4Error *outError) {
            alloc_slice body;
            if (rq.deltaCB == nullptr) {
                body = (rq.allocedBody.buf)? rq.allocedBody : alloc_slice(rq.body);
            } else {
                // Apply a delta via a callback:
                slice delta = (rq.allocedBody.buf)? slice(rq.allocedBody) : slice(rq.body);
                if (!rq.deltaSourceRevID.buf || !selectRevision(rq.deltaSourceRevID, true)) {
                    if (outError)
                        *outError = c4error_printf(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                        "Missing source revision '%.*s' for delta",
                                        SPLAT(rq.deltaSourceRevID));
                    return nullptr;
                } else if (!getSelectedRevBody()) {
                    if (outError)
                        *outError = c4error_printf(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                        "Missing body of source revision '%.*s' for delta",
                                        SPLAT(rq.deltaSourceRevID));
                    return nullptr;
                } else {
                    body = rq.deltaCB(rq.deltaCBContext, this, delta, outError);
                }
            }
            return _newProperties(body);
        }


        fleece::Doc _newProperties(alloc_slice body) {
            if (body.size > 0)
                database()->validateRevisionBody(body);
            else
                body = alloc_slice{(FLDict)Dict::emptyDict(), 2};
            Doc fldoc = Doc(body, kFLUntrusted, (FLSharedKeys)database()->documentKeys());
            Assert(fldoc.asDict());     // validateRevisionBody should have preflighted this
            return fldoc;
        }


        // Handles `c4doc_put` when `rq.existingRevision` is false (a regular save.)
        // The caller has already done most of the checking, incl. MVCC.
        bool putNewRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            // Update the flags:
            Revision newRev;
            newRev.flags = convertNewRevisionFlags(rq.revFlags);

            // Update the version vector:
            auto newVers = _currentVersionVector();
            newVers.incrementGen(kMePeerID);
            alloc_slice newRevID = newVers.asBinary();
            newRev.revID = revid(newRevID);

            // Update the local body:
            C4Error err;
            Doc fldoc = _newProperties(rq, &err);
            if (!fldoc)
                return false;
            newRev.properties = fldoc.asDict();

            _db->dataFile()->_logVerbose("putNewRevision '%.*s' %s ; currently %s",
                    SPLAT(docID),
                    string(newVers.asASCII()).c_str(),
                    string(_currentVersionVector().asASCII()).c_str());

            // Store in VectorRecord, and update C4Document properties:
            _doc.setCurrentRevision(newRev);
            _selectRemote(RemoteID::Local);
            return _saveNewRev(rq, newRev, outError);
        }


        // Handles `c4doc_put` when `rq.existingRevision` is true (called by the Pusher)
        int32_t putExistingRevision(const C4DocPutRequest &rq, C4Error *outError) override {
            Revision newRev;
            newRev.flags = convertNewRevisionFlags(rq.revFlags);
            Doc fldoc = _newProperties(rq, outError);
            if (!fldoc)
                return -1;
            newRev.properties = fldoc.asDict();

            // Parse the history array:
            VersionVector newVers;
            newVers.readHistory((slice*)rq.history, rq.historyCount, myPeerID());
            alloc_slice newVersBinary = newVers.asBinary();
            newRev.revID = revid(newVersBinary);

            // Does it fit the current revision?
            auto remote = RemoteID(rq.remoteDBID);
            int commonAncestor = 1;
            auto order = kNewer;
            if (_doc.exists()) {
                // See whether to update the local revision:
                order = newVers.compareTo(_currentVersionVector());
            }

            // Log the update. Normally verbose, but a conflict is info (if from the replicator)
            // or error (if local).
            if (DBLog.willLog(LogLevel::Verbose) || order == kConflicting) {
                static constexpr const char* kOrderName[4] = {"same", "older", "newer", "conflict"};
                alloc_slice newVersStr = newVers.asASCII();
                alloc_slice oldVersStr = _currentVersionVector().asASCII();
                if (order != kConflicting)
                    _db->dataFile()->_logVerbose(
                        "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> %s (remote %d)",
                        SPLAT(docID), SPLAT(newVersStr), SPLAT(oldVersStr),
                        kOrderName[order], rq.remoteDBID);
                else if (remote != RemoteID::Local)
                    _db->dataFile()->_logInfo(
                        "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> conflict (remote %d)",
                        SPLAT(docID), SPLAT(newVersStr), SPLAT(oldVersStr), rq.remoteDBID);
                else
                    _db->dataFile()->_logWarning(
                        "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> conflict (remote %d)",
                        SPLAT(docID), SPLAT(newVersStr), SPLAT(oldVersStr), rq.remoteDBID);
            }

            switch (order) {
                case kSame:
                case kOlder:
                    // I already have this revision, don't update local
                    commonAncestor = 0;
                    break;
                case kNewer:
                    // It's newer, so update local to this revision:
                    _doc.setCurrentRevision(newRev);
                    break;
                case kConflicting:
                    // Conflict, so update only the remote (if any):
                    if (remote == RemoteID::Local) {
                        c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                        return -1;
                    }
                    newRev.flags |= DocumentFlags::kConflicted;
                    break;
            }
            
            if (remote != RemoteID::Local) {
                // If this is a revision from a remote, update it in the doc:
                _doc.setRemoteRevision(remote, newRev);
            }

            // Update C4Document.selectedRev:
            _selectRemote(remote);

            // Save to DB, if requested:
            if (!_saveNewRev(rq, newRev, outError))
                return -1;

            return commonAncestor;
        }


        bool _saveNewRev(const C4DocPutRequest &rq, const Revision &newRev, C4Error *outError) {
            if (rq.save && !save()) {
                c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                return false;
            }
            return true;
        }


        void resolveConflict(C4String winningRevID,
                             C4String losingRevID,
                             C4Slice mergedBody,
                             C4RevisionFlags mergedFlags,
                             bool /*pruneLosingBranch*/) override
        {
            optional<pair<RemoteID, Revision>> won = _findRemote(winningRevID),
                                              lost = _findRemote(losingRevID);
            if (!won || !lost)
                error::_throw(error::NotFound, "Revision not found");
            if (won->first == lost->first)
                error::_throw(error::InvalidParameter, "That's the same revision");

            // One has to be Local, the other has to be remote and a conflict:
            Revision localRev, remoteRev;
            RemoteID remoteID;
            bool localWon = won->first == RemoteID::Local;
            if (localWon) {
                localRev = won->second;
                tie(remoteID, remoteRev) = *lost;
            } else if (lost->first == RemoteID::Local) {
                localRev = lost->second;
                tie(remoteID, remoteRev) = *won;
            } else {
                error::_throw(error::Conflict, "Conflict must involve the local revision");
            }
            if (!(remoteRev.flags & DocumentFlags::kConflicted))
                error::_throw(error::Conflict, "Revisions are not in conflict");

            // Construct a merged version vector:
            VersionVector localVersion = localRev.versionVector(),
                          remoteVersion = remoteRev.versionVector(),
                          mergedVersion;
            if (!localWon && !mergedBody.buf
                                    && !localVersion.isNewerIgnoring(kMePeerID, remoteVersion)) {
                // If there's no new merged body, and the local revision lost,
                // and its only changes not in the remote version are by me, then
                // just get rid of the local version and keep the remote one.
                // TODO: This assumes the local rev hasn't been pushed anywhere yet.
                //       If that's not true, then we should create a new version;
                //       but currently there's no way of knowing.
                mergedVersion = remoteVersion;
            } else {
                if (localWon)
                    mergedVersion = localVersion.mergedWith(remoteVersion);
                else
                    mergedVersion = remoteVersion.mergedWith(localVersion);
                // We have to increment something to get a genuinely new version vector.
                mergedVersion.incrementGen(kMePeerID);
            }
            alloc_slice mergedRevID = mergedVersion.asBinary();

            // Update the local/current revision with the resulting merge:
            Doc mergedDoc;
            localRev.revID = revid(mergedRevID);
            if (mergedBody.buf) {
                mergedDoc = _newProperties(alloc_slice(mergedBody));
                localRev.properties = mergedDoc.asDict();
                localRev.flags = convertNewRevisionFlags(mergedFlags);
            } else {
                localRev.properties = won->second.properties;
                localRev.flags = won->second.flags - DocumentFlags::kConflicted;
            }
            _doc.setCurrentRevision(localRev);

            // Remote rev is no longer a conflict:
            remoteRev.flags = remoteRev.flags - DocumentFlags::kConflicted;
            _doc.setRemoteRevision(remoteID, remoteRev);

            _updateDocFields();
            _selectRemote(RemoteID::Local);
            LogTo(DBLog, "Resolved conflict in '%.*s' between #%s and #%s -> #%s",
                  SPLAT(docID),
                  string(localVersion.asASCII()).c_str(),
                  string(remoteVersion.asASCII()).c_str(),
                  string(mergedVersion.asASCII()).c_str() );
        }


        bool save(unsigned maxRevTreeDepth =0) override {
            requireValidDocID();
            switch (_doc.save(database()->transaction())) {
                case VectorRecord::kNoSave:
                    return true;
                case VectorRecord::kNoNewSequence:
                    _updateDocFields();  // flags may have changed
                    return true;
                case VectorRecord::kConflict:
                    return false;
                case VectorRecord::kNewSequence:
                    _updateDocFields();
                    _selectRemote(RemoteID::Local);
                    if (_doc.sequence() > sequence)
                        sequence = selectedRev.sequence = _doc.sequence();
                    if (_db->dataFile()->willLog(LogLevel::Verbose)) {
                        alloc_slice revID = _doc.revID().expanded();
                        _db->dataFile()->_logVerbose( "%-s '%.*s' rev #%.*s as seq %" PRIu64,
                                                     ((flags & kRevDeleted) ? "Deleted" : "Saved"),
                                                     SPLAT(docID), SPLAT(revID), sequence);
                    }
                    database()->documentSaved(this);
                    return true;
            }
            return false; // unreachable
        }


    private:
        VectorRecord        _doc;
        optional<RemoteID>  _remoteID;    // Identifies selected revision
        fleece::Doc         _latestBody;  // Holds onto latest Fleece body I created
    };


#pragma mark - FACTORY:


    Retained<Document> VectorDocumentFactory::newDocumentInstance(C4Slice docID, ContentOption c) {
        return new VectorDocument(database(), docID, c);
    }


    Retained<Document> VectorDocumentFactory::newDocumentInstance(const Record &record) {
        return new VectorDocument(database(), record);
    }


    Document* VectorDocumentFactory::documentContaining(FLValue value) {
        if (auto nuDoc = VectorRecord::containing(value); nuDoc)
            return (VectorDocument*)nuDoc->owner;
        else
            return nullptr;
    }


    vector<alloc_slice> VectorDocumentFactory::findAncestors(const vector<slice> &docIDs,
                                                             const vector<slice> &revIDs,
                                                             unsigned maxAncestors,
                                                             bool mustHaveBodies,
                                                             C4RemoteID remoteDBID)
    {
        // Map docID->revID for faster lookup in the callback:
        unordered_map<slice,slice> revMap(docIDs.size());
        for (ssize_t i = docIDs.size() - 1; i >= 0; --i)
            revMap[docIDs[i]] = revIDs[i];
        const peerID myPeerID {database()->myPeerID()};

        // These variables get reused in every call to the callback but are declared outside to
        // avoid multiple construct/destruct calls:
        stringstream result;
        VersionVector localVec, requestedVec;

        // Subroutine to compare a local version with the requested one:
        auto compareLocalRev = [&](slice revVersion) -> versionOrder {
            localVec.readBinary(revVersion);
            return localVec.compareTo(requestedVec);
        };

        auto callback = [&](const RecordUpdate &rec) -> alloc_slice {
            // --- This callback runs inside the SQLite query ---
            // --- It will be called once for each existing requested docID, in arbitrary order ---

            // Look up matching requested revID, and convert to encoded binary form:
            requestedVec.readASCII(revMap[rec.key], myPeerID);

            // Check whether the doc's current rev is this version, or a newer, or a conflict:
            auto cmp = compareLocalRev(rec.version);
            auto status = C4FindDocAncestorsResultFlags(cmp);

            // Check whether this revID matches any of the doc's remote revisions:
            if (remoteDBID != 0) {
                VectorRecord::forAllRevIDs(rec, [&](RemoteID remote, revid aRev, bool hasBody) {
                    if (remote > RemoteID::Local && compareLocalRev(aRev) == kSame) {
                        if (hasBody)
                            status |= kRevsHaveLocal;
                        if (remote == RemoteID(remoteDBID))
                            status |= kRevsAtThisRemote;
                    }
                });
            }

            char statusChar = '0' + char(status);
            if (cmp == kNewer || cmp == kSame) {
                // If I already have this revision, just return the status byte:
                return alloc_slice(&statusChar, 1);
            }

            // I don't have the requested rev, so find revs that could be ancestors of it,
            // and append them as a JSON array:
            result.str("");
            result << statusChar << '[';

            std::set<alloc_slice> added;
            delimiter delim(",");
            VectorRecord::forAllRevIDs(rec, [&](RemoteID, revid aRev, bool hasBody) {
                if (delim.count() < maxAncestors && hasBody >= mustHaveBodies) {
                    if (!(compareLocalRev(aRev) & kNewer)) {
                        alloc_slice vector = localVec.asASCII(myPeerID);
                        if (added.insert(vector).second)            // [skip duplicate vectors]
                            result << delim << '"' << vector << '"';
                    }
                }
            });

            result << ']';
            return alloc_slice(result.str());                       // --> Done!
        };
        return database()->dataFile()->defaultKeyStore().withDocBodies(docIDs, callback);
    }


}
