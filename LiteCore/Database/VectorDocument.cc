//
// VectorDocument.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "VectorDocument.hh"
#include "VectorRecord.hh"
#include "VersionVector.hh"
#include "CollectionImpl.hh"
#include "c4Database.hh"
#include "DatabaseImpl.hh"
#include "Error.hh"
#include "Delimiter.hh"
#include "StringUtil.hh"
#include <set>

namespace litecore {
    using namespace std;
    using namespace fleece;
    using namespace litecore;

    class VectorDocument final
        : public C4Document
        , public InstanceCountedIn<VectorDocument> {
      public:
        VectorDocument(C4Collection* coll, slice docID, ContentOption whichContent)
            : C4Document(coll, alloc_slice(docID)), _doc(keyStore(), docID, whichContent) {
            _initialize();
        }

        VectorDocument(C4Collection* coll, const Record& doc) : C4Document(coll, doc.key()), _doc(keyStore(), doc) {
            _initialize();
        }

        VectorDocument(const VectorDocument& other) : C4Document(other), _doc(other._doc), _remoteID(other._remoteID) {}

        Retained<C4Document> copy() const override { return new VectorDocument(*this); }

        ~VectorDocument() override { _doc.owner = nullptr; }

        void _initialize() {
            _doc.owner = this;
            _doc.setEncoder(database()->sharedFleeceEncoder());
            _updateDocFields();
            _selectRemote(RemoteID::Local);
        }

        void _updateDocFields() {
            _revID = _expandRevID(_doc.revID());

            _flags = C4DocumentFlags(_doc.flags());
            if ( _doc.exists() ) _flags |= kDocExists;
            _sequence = _doc.sequence();
        }

        SourceID mySourceID() const { return SourceID{asInternal(database())->mySourceID()}; }

        static alloc_slice _expandRevID(revid rev, SourceID myID = kMeSourceID) {
            if ( !rev ) return nullslice;
            else if ( rev.isVersion() )
                return rev.asVersion().asASCII(myID);
            else
                return rev.expanded();
        }

        revidBuffer _parseRevID(slice revID) const {
            if ( revID ) {
                revidBuffer binaryID(revID);
                if ( binaryID.getRevID().isVersion() ) {
                    // If it's a version in global form, convert it to local form:
                    if ( auto vers = binaryID.getRevID().asVersion(); vers.author() == mySourceID() )
                        binaryID = Version(vers.time(), kMeSourceID);
                }
                return binaryID;
            }
            error::_throw(error::BadRevisionID, "Not a version string: '%.*s'", SPLAT(revID));
        }

#pragma mark - SELECTING REVISIONS:

        optional<pair<RemoteID, Revision>> _findRemote(slice asciiRevID) {
            RemoteID remote = RemoteID::Local;
            if ( asciiRevID.findByte(',') ) {
                // It's a version vector; look for an exact match:
                VersionVector vers   = VersionVector::fromASCII(asciiRevID, mySourceID());
                alloc_slice   binary = vers.asBinary();
                while ( auto rev = _doc.loadRemoteRevision(remote) ) {
                    if ( rev->revID == binary ) return {{remote, *rev}};
                    remote = _doc.loadNextRemoteID(remote);
                }
            } else {
                revidBuffer buf   = _parseRevID(asciiRevID);
                revid       revID = buf.getRevID();
                if ( revID.isVersion() ) {
                    // It's a single version, so find a vector that starts with it:
                    Version vers = revID.asVersion();
                    while ( auto rev = _doc.loadRemoteRevision(remote) ) {
                        if ( rev->revID && rev->version() == vers ) return {{remote, *rev}};
                        remote = _doc.loadNextRemoteID(remote);
                    }
                } else {
                    while ( auto rev = _doc.loadRemoteRevision(remote) ) {
                        if ( rev->revID == revID ) return {{remote, *rev}};
                        remote = _doc.loadNextRemoteID(remote);
                    }
                }
            }
            return nullopt;
        }

        // intentionally does not load other revisions ... throws if they're not in memory.
        // Calling code should be fixed to load the doc with all revisions using c4db_getDoc2.
        bool _selectRemote(RemoteID remote) {
            if ( auto rev = _doc.remoteRevision(remote); rev && rev->revID ) {
                return _selectRemote(remote, *rev);
            } else {
                _remoteID = nullopt;
                clearSelectedRevision();
                return false;
            }
        }

        bool _selectRemote(RemoteID remote, Revision& rev) {
            _remoteID          = remote;
            _selectedRevID     = _expandRevID(rev.revID);
            _selected.revID    = _selectedRevID;
            _selected.sequence = _doc.sequence();  // VectorRecord doesn't have per-rev sequence

            _selected.flags = 0;
            if ( remote == RemoteID::Local ) _selected.flags |= kRevLeaf;
            if ( rev.isDeleted() ) _selected.flags |= kRevDeleted;
            if ( rev.hasAttachments() ) _selected.flags |= kRevHasAttachments;
            if ( rev.isConflicted() ) _selected.flags |= kRevIsConflict | kRevLeaf;
            return true;
        }

        bool selectRevision(slice revID, bool withBody) override {
            if ( auto r = _findRemote(revID); r ) {
                return _selectRemote(r->first, r->second);
            } else {
                _remoteID = nullopt;
                clearSelectedRevision();
                return false;
            }
        }

        optional<Revision> _selectedRevision() const { return _remoteID ? _doc.remoteRevision(*_remoteID) : nullopt; }

        bool selectCurrentRevision() noexcept override { return _selectRemote(RemoteID::Local); }

        bool selectNextRevision() override { return _remoteID && _selectRemote(_doc.nextRemoteID(*_remoteID)); }

        bool selectNextLeafRevision(bool includeDeleted, bool withBody) override {
            while ( selectNextRevision() ) {
                if ( _selected.flags & kRevLeaf ) return !withBody || loadRevisionBody();
            }
            return false;
        }

#pragma mark - ACCESSORS:

        slice getRevisionBody() const noexcept override {
            if ( auto rev = _selectedRevision() ) {
                // Current revision, or remote with the same version:
                if ( rev->revID == _doc.revID() ) {
                    if ( _doc.contentAvailable() >= kCurrentRevOnly ) return _doc.currentRevisionData();
                } else if ( rev->properties ) {
                    // Else the properties have to be re-encoded to a slice:
                    SharedEncoder enc(database()->sharedFleeceEncoder());
                    enc << rev->properties;
                    _latestBody = enc.finishDoc();
                    return _latestBody.data();
                    ;
                }
            }
            return nullslice;
        }

        FLDict getProperties() const noexcept override {
            auto rev = _selectedRevision();
            return rev ? rev->properties : nullptr;
        }

        alloc_slice getSelectedRevIDGlobalForm() const override {
            if ( auto rev = _selectedRevision(); rev ) return rev->versionVector().asASCII(mySourceID());
            else
                return nullslice;
        }

        alloc_slice getRevisionHistory(unsigned maxRevs, const slice backToRevs[],
                                       unsigned backToRevsCount) const override {
            if ( auto rev = _selectedRevision(); rev ) {
                if ( _doc.versioning() == Versioning::Vectors ) {
                    VersionVector vers = rev->versionVector();
                    if ( maxRevs > 0 && vers.count() > maxRevs ) vers.prune(maxRevs);
                    // Easter egg: if maxRevs is 0, don't replace '*' with my peer ID [tests use this]
                    return vers.asASCII(maxRevs ? mySourceID() : kMeSourceID);
                } else {
                    string history = rev->revID.str();
                    if ( _remoteID == RemoteID::Local ) {
                        if ( RemoteID parID = _doc.legacyTreeParent(); parID != RemoteID::Local ) {
                            auto parent = _doc.remoteRevision(parID);
                            if ( parent && parent->revID != rev->revID ) { history += ", " + parent->revID.str(); }
                        }
                    }
                    return alloc_slice(history);
                }
            } else {
                return nullslice;
            }
        }

        alloc_slice remoteAncestorRevID(C4RemoteID remote) override {
            if ( auto rev = _doc.loadRemoteRevision(RemoteID(remote)) ) return rev->revID.expanded();
            return nullptr;
        }

        void setRemoteAncestorRevID(C4RemoteID remote, slice revID) override {
            Assert(RemoteID(remote) != RemoteID::Local);
            Revision    revision;
            revidBuffer vers(revID);
            if ( auto r = _findRemote(revID); r ) revision = r->second;
            else
                revision.revID = vers.getRevID();
            _doc.setRemoteRevision(RemoteID(remote), revision);
        }

        bool isRevRejected() override {
            auto rev = _selectedRevision();
            return rev && (rev->flags & DocumentFlags::kRejected);
        }

        void revIsRejected(slice revID) override {
            if ( auto rev = _findRemote(revID) ) rev->second.flags |= DocumentFlags::kRejected;
        }

#pragma mark - EXISTENCE / LOADING:

        bool exists() const override { return _doc.exists(); }

        [[nodiscard]] bool loadRevisions() const override {
            return _doc.contentAvailable() >= kEntireBody || const_cast<VectorRecord&>(_doc).loadData(kEntireBody);
        }

        bool revisionsLoaded() const noexcept override { return _doc.contentAvailable() >= kEntireBody; }

        bool hasRevisionBody() const noexcept override { return _doc.exists() && _remoteID; }

        [[nodiscard]] bool loadRevisionBody() const override {
            if ( !_remoteID ) return false;
            auto which = (*_remoteID == RemoteID::Local) ? kCurrentRevOnly : kEntireBody;
            return const_cast<VectorRecord&>(_doc).loadData(which);
        }

#pragma mark - UPDATING:

        VersionVector _currentVersionVector() {
            auto curRevID = _doc.revID();
            return curRevID ? curRevID.asVersionVector() : VersionVector();
        }

        static DocumentFlags convertNewRevisionFlags(C4RevisionFlags revFlags) {
            DocumentFlags docFlags = {};
            if ( revFlags & kRevDeleted ) docFlags |= DocumentFlags::kDeleted;
            if ( revFlags & kRevHasAttachments ) docFlags |= DocumentFlags::kHasAttachments;
            return docFlags;
        }

        // Warning: we cast away const of rq to have rq.revFlags updated by deltaCB.
        fleece::Doc _newProperties(const C4DocPutRequest& rq, C4Error* outError) {
            alloc_slice body;
            if ( rq.deltaCB == nullptr ) {
                body = (rq.allocedBody.buf) ? alloc_slice(rq.allocedBody) : alloc_slice(rq.body);
            } else {
                // Apply a delta via a callback:
                slice delta = (rq.allocedBody.buf) ? slice(rq.allocedBody) : slice(rq.body);
                if ( !rq.deltaSourceRevID.buf || !selectRevision(rq.deltaSourceRevID, true) ) {
                    if ( outError )
                        *outError =
                                c4error_printf(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                               "Missing source revision '%.*s' for delta", SPLAT(rq.deltaSourceRevID));
                    return nullptr;
                } else if ( !getRevisionBody() ) {
                    if ( outError )
                        *outError = c4error_printf(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                                   "Missing body of source revision '%.*s' for delta",
                                                   SPLAT(rq.deltaSourceRevID));
                    return nullptr;
                } else {
                    body = rq.deltaCB(rq.deltaCBContext, this, delta, const_cast<C4RevisionFlags*>(&rq.revFlags),
                                      outError);
                }
            }
            return _newProperties(body);
        }

        fleece::Doc _newProperties(alloc_slice body) {
            if ( body.size > 0 ) asInternal(database())->validateRevisionBody(body);
            else
                body = alloc_slice{(FLDict)Dict::emptyDict(), 2};
            Doc fldoc = Doc(body, kFLUntrusted, database()->getFleeceSharedKeys());
            Assert(fldoc.asDict());  // validateRevisionBody should have preflighted this
            return fldoc;
        }

        // Handles `c4doc_put` when `rq.existingRevision` is false (a regular save.)
        // The caller has already done most of the checking, incl. MVCC.
        bool putNewRevision(const C4DocPutRequest& rq, C4Error* outError) override {
            _doc.upgradeVersioning();

            // Update the flags:
            Revision newRev;
            newRev.flags = convertNewRevisionFlags(rq.revFlags);

            // Update the version vector:
            auto newVers = _currentVersionVector();
            newVers.addNewVersion(asInternal(database())->versionClock());
            alloc_slice newRevID = newVers.asBinary();
            newRev.revID         = revid(newRevID);

            // Update the local body:
            C4Error err;
            Doc     fldoc = _newProperties(rq, &err);
            if ( !fldoc ) return false;
            newRev.properties = fldoc.asDict();

            keyStore().dataFile()._logVerbose("putNewRevision '%.*s' %s ; currently %s", SPLAT(_docID),
                                              string(newVers.asASCII()).c_str(),
                                              string(_currentVersionVector().asASCII()).c_str());

            // Store in VectorRecord, and update C4Document properties:
            _doc.setCurrentRevision(newRev);
            _selectRemote(RemoteID::Local);
            return _saveIfRequested(rq, outError);
        }

        // Handles `c4doc_put` when `rq.existingRevision` is true (called by the Pusher)
        int32_t putExistingRevision(const C4DocPutRequest& rq, C4Error* outError) override {
            _doc.upgradeVersioning();

            Revision newRev;
            newRev.flags = convertNewRevisionFlags(rq.revFlags);
            Doc fldoc    = _newProperties(rq, outError);
            if ( !fldoc ) return -1;
            newRev.properties = fldoc.asDict();

            // Parse the version vector from the history, and use it to update the db's clock:
            VersionVector newVers;
            newVers.readHistory((slice*)rq.history, rq.historyCount, mySourceID());
            if ( !newVers.updateClock(asInternal(database())->versionClock()) ) {
                if ( outError ) {
                    alloc_slice vecStr = newVers.asASCII();
                    *outError          = c4error_printf(LiteCoreDomain, kC4ErrorBadRevisionID,
                                                        "Invalid timestamp in version vector %.*s", SPLAT(vecStr));
                }
                return -1;
            }

            alloc_slice newVersBinary = newVers.asBinary();
            newRev.revID              = revid(newVersBinary);

            // Does it fit the current revision?
            auto remote         = RemoteID(rq.remoteDBID);
            int  commonAncestor = 1;
            auto order          = kNewer;
            if ( _doc.exists() ) {
                // See whether to update the local revision:
                order = newVers.compareTo(_currentVersionVector());
            }

            // Log the update. Normally verbose, but a conflict is info (if from the replicator)
            // or error (if local).
            if ( DBLog.willLog(LogLevel::Verbose) || order == kConflicting ) {
                static constexpr const char* kOrderName[4] = {"same", "older", "newer", "conflict"};
                alloc_slice                  newVersStr    = newVers.asASCII();
                alloc_slice                  oldVersStr    = _currentVersionVector().asASCII();
                if ( order != kConflicting )
                    keyStore().dataFile()._logVerbose(
                            "putExistingRevision '%.*s' #%.*s ; currently #%.*s --> %s (remote %d)", SPLAT(_docID),
                            SPLAT(newVersStr), SPLAT(oldVersStr), kOrderName[order], rq.remoteDBID);
                else if ( remote != RemoteID::Local )
                    keyStore().dataFile()._logInfo("putExistingRevision '%.*s' #%.*s ; currently "
                                                   "#%.*s --> conflict (remote %d)",
                                                   SPLAT(_docID), SPLAT(newVersStr), SPLAT(oldVersStr), rq.remoteDBID);
                else
                    keyStore().dataFile()._logWarning("putExistingRevision '%.*s' #%.*s ; "
                                                      "currently #%.*s --> conflict (remote %d)",
                                                      SPLAT(_docID), SPLAT(newVersStr), SPLAT(oldVersStr),
                                                      rq.remoteDBID);
            }

            switch ( order ) {
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
                    if ( !rq.allowConflict || remote == RemoteID::Local ) {
                        c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                        return -1;
                    }
                    newRev.flags |= DocumentFlags::kConflicted;
                    break;
            }

            if ( remote != RemoteID::Local ) {
                // If this is a revision from a remote, update it in the doc:
                _doc.setRemoteRevision(remote, newRev);
            }

            // Update C4Document.selectedRev:
            _selectRemote(remote);

            // Save to DB, if requested:
            if ( !_saveIfRequested(rq, outError) ) return -1;

            return commonAncestor;
        }

        bool _saveIfRequested(const C4DocPutRequest& rq, C4Error* outError) {
            if ( rq.save && !save() ) {
                c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                return false;
            }
            return true;
        }

        void resolveConflict(slice winningRevID, slice losingRevID, slice mergedBody, C4RevisionFlags mergedFlags,
                             bool /*pruneLosingBranch*/) override {
            optional<pair<RemoteID, Revision>> won = _findRemote(winningRevID), lost = _findRemote(losingRevID);
            if ( !won || !lost ) error::_throw(error::NotFound, "Revision not found");
            if ( won->first == lost->first ) error::_throw(error::InvalidParameter, "That's the same revision");

            // One has to be Local, the other has to be remote and a conflict:
            Revision localRev, remoteRev;
            RemoteID remoteID;
            bool     localWon = won->first == RemoteID::Local;
            if ( localWon ) {
                localRev                 = won->second;
                tie(remoteID, remoteRev) = *lost;
            } else if ( lost->first == RemoteID::Local ) {
                localRev                 = lost->second;
                tie(remoteID, remoteRev) = *won;
            } else {
                error::_throw(error::Conflict, "Conflict must involve the local revision");
            }
            if ( !(remoteRev.flags & DocumentFlags::kConflicted) )
                error::_throw(error::Conflict, "Revisions are not in conflict");

            // Construct a merged version vector:
            VersionVector localVersion = localRev.versionVector(), remoteVersion = remoteRev.versionVector(),
                          mergedVersion;
            if ( !localWon && !mergedBody.buf && !localVersion.isNewerIgnoring(kMeSourceID, remoteVersion) ) {
                // If there's no new merged body, and the local revision lost,
                // and its only changes not in the remote version are by me, then
                // just get rid of the local version and keep the remote one.
                // TODO: This assumes the local rev hasn't been pushed anywhere yet.
                //       If that's not true, then we should create a new version;
                //       but currently there's no way of knowing.
                mergedVersion = remoteVersion;
            } else {
                mergedVersion =
                        VersionVector::merge(localVersion, remoteVersion, asInternal(database())->versionClock());
            }
            alloc_slice mergedRevID = mergedVersion.asBinary();

            // Update the local/current revision with the resulting merge:
            Doc mergedDoc;
            localRev.revID = revid(mergedRevID);
            if ( mergedBody.buf ) {
                mergedDoc           = _newProperties(alloc_slice(mergedBody));
                localRev.properties = mergedDoc.asDict();
                localRev.flags      = convertNewRevisionFlags(mergedFlags);
            } else {
                localRev.properties = won->second.properties;
                localRev.flags      = won->second.flags - DocumentFlags::kConflicted;
            }
            _doc.setCurrentRevision(localRev);

            // Remote rev is no longer a conflict:
            remoteRev.flags = remoteRev.flags - DocumentFlags::kConflicted;
            _doc.setRemoteRevision(remoteID, remoteRev);

            _updateDocFields();
            _selectRemote(RemoteID::Local);
            LogTo(DBLog, "Resolved conflict in '%.*s' between #%s and #%s -> #%s", SPLAT(_docID),
                  string(localVersion.asASCII()).c_str(), string(remoteVersion.asASCII()).c_str(),
                  string(mergedVersion.asASCII()).c_str());
        }

        bool save(unsigned /*maxRevTreeDepth*/ = 0) override {
            requireValidDocID(_docID);
            auto db = asInternal(database());
            db->mustBeInTransaction();
            switch ( _doc.save(db->transaction(), db->versionClock()) ) {
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
                    if ( _doc.sequence() > _sequence ) _sequence = _selected.sequence = _doc.sequence();
                    if ( db->dataFile()->willLog(LogLevel::Verbose) ) {
                        alloc_slice revID = _doc.revID().expanded();
                        db->dataFile()->_logVerbose("%-s '%.*s' rev #%.*s as seq %" PRIu64,
                                                    ((_flags & kRevDeleted) ? "Deleted" : "Saved"), SPLAT(_docID),
                                                    SPLAT(revID), (uint64_t)_sequence);
                    }
                    asInternal(collection())->documentSaved(this);
                    return true;
            }
            return false;  // unreachable
        }


      private:
        VectorRecord        _doc;
        optional<RemoteID>  _remoteID;    // Identifies selected revision
        mutable fleece::Doc _latestBody;  // Holds onto latest Fleece body I created
    };

#pragma mark - FACTORY:

    Retained<C4Document> VectorDocumentFactory::newDocumentInstance(slice docID, ContentOption c) {
        return new VectorDocument(collection(), docID, c);
    }

    Retained<C4Document> VectorDocumentFactory::newDocumentInstance(const Record& record) {
        return new VectorDocument(collection(), record);
    }

    C4Document* VectorDocumentFactory::documentContaining(FLValue value) {
        if ( auto nuDoc = VectorRecord::containing(value); nuDoc ) return (VectorDocument*)nuDoc->owner;
        else
            return nullptr;
    }

    vector<alloc_slice> VectorDocumentFactory::findAncestors(const vector<slice>& docIDs, const vector<slice>& revIDs,
                                                             unsigned maxAncestors, bool mustHaveBodies,
                                                             C4RemoteID remoteDBID) {
        // Map docID->revID for faster lookup in the callback:
        unordered_map<slice, slice> revMap(docIDs.size());
        for ( ssize_t i = static_cast<ssize_t>(docIDs.size()) - 1; i >= 0; --i ) revMap[docIDs[i]] = revIDs[i];
        const SourceID mySourceID{asInternal(collection()->getDatabase())->mySourceID()};

        // These variables get reused in every call to the callback but are declared outside to
        // avoid multiple construct/destruct calls:
        stringstream  result;
        VersionVector localVec, requestedVec;

        // Subroutine to compare a local version with the requested one:
        auto compareLocalRev = [&](slice revVersion) -> versionOrder {
            localVec.readBinary(revVersion);
            return localVec.compareTo(requestedVec);
        };

        auto callback = [&](const RecordUpdate& rec) -> alloc_slice {
            // --- This callback runs inside the SQLite query ---
            // --- It will be called once for each existing requested docID, in arbitrary order ---

            // Look up matching requested revID, and convert to encoded binary form:
            requestedVec.readASCII(revMap[rec.key], mySourceID);

            // Check whether the doc's current rev is this version, or a newer, or a conflict:
            bool recUsesVVs = revid(rec.version).isVersion();
            if ( recUsesVVs ) {
                localVec.readBinary(rec.version);
            } else {
                // Doc still has a legacy tree-based revID. Convert to a VV
                localVec = VectorRecord::createLegacyVersionVector(rec);
            }
            auto cmp    = localVec.compareTo(requestedVec);
            auto status = C4FindDocAncestorsResultFlags(cmp);

            // Check whether this revID matches any of the doc's remote revisions:
            if ( remoteDBID != 0 ) {
                VectorRecord::forAllRevIDs(rec, [&](RemoteID remote, revid aRev, bool hasBody) {
                    if ( remote > RemoteID::Local && compareLocalRev(aRev) == kSame ) {
                        if ( hasBody ) status |= kRevsHaveLocal;
                        if ( remote == RemoteID(remoteDBID) ) status |= kRevsAtThisRemote;
                    }
                });
            }

            char statusChar = static_cast<char>('0' + char(status));
            if ( cmp == kNewer || cmp == kSame ) {
                // If I already have this revision, just return the status byte:
                return {&statusChar, 1};
            }

            // I don't have the requested rev, so find revs that could be ancestors of it,
            // and append them as a JSON array:
            result.str("");
            result << statusChar << '[';

            std::set<alloc_slice> added;
            delimiter             delim(",");
            VectorRecord::forAllRevIDs(rec, [&](RemoteID, revid aRev, bool hasBody) {
                if ( delim.count() < maxAncestors && hasBody >= mustHaveBodies ) {
                    if ( !(compareLocalRev(aRev) & kNewer) ) {
                        alloc_slice vector = localVec.asASCII(mySourceID);
                        if ( added.insert(vector).second )  // [skip duplicate vectors]
                            result << delim << '"' << vector << '"';
                    }
                }
            });

            result << ']';
            return alloc_slice(result.str());  // --> Done!
        };
        return asInternal(collection())->keyStore().withDocBodies(docIDs, callback);
    }


}  // namespace litecore
