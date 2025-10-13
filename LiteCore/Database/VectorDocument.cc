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
#include "VersionVectorWithLegacy.hh"
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

    static VersionVector toVersionVector(const Revision& rev) {
        if ( rev.hasVersionVector() ) { return rev.versionVector(); }

        VersionVector v;
        v.add(Version::legacyVersion(rev.revID));
        return v;
    }

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
                        if ( rev->hasVersionVector() && rev->version() == vers ) return {{remote, *rev}};
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
        // Calling code should be fixed to load the doc with all revisions using c4coll_getDoc.
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
            if ( auto rev = _selectedRevision(); rev ) {
                if ( rev->hasVersionVector() ) return rev->versionVector().asASCII(mySourceID());
                else
                    return C4Document::getSelectedRevIDGlobalForm();
            } else
                return nullslice;
        }

        alloc_slice getRevisionHistory(unsigned maxRevs, const slice backToRevs[],
                                       unsigned backToRevsCount) const override {
            alloc_slice result;
            if ( auto rev = _selectedRevision(); rev ) {
                // First get the version vector of the selected revision:
                VersionVecWithLegacy vvl(rev->revID);
                if ( (backToRevsCount > 0 || _doc.lastLegacyRevID() || !vvl.legacy.empty()) && loadRevisions() ) {
                    // If current rev or peer have legacy revids, look for legacy ancestors:
                    if ( _remoteID == RemoteID::Local ) {
                        // Start with the doc's last legacy rev:
                        if ( revid lastRevID = _doc.lastLegacyRevID() ) vvl.legacy.emplace_back(lastRevID);
                    }

                    // Search the remotes for earlier legacy revs that aren't conflicts:
                    unsigned curGen = UINT_MAX;
                    if ( !vvl.legacy.empty() ) curGen = revid(vvl.legacy.back()).generation();
                    _doc.forAllRevs([&](RemoteID rem, Revision const& otherRev) {
                        if ( !otherRev.revID.isVersion() && otherRev.revID.generation() < curGen
                             && !(otherRev.flags & DocumentFlags::kConflicted) ) {
                            vvl.legacy.emplace_back(otherRev.revID);
                        }
                    });

                    // Sort legacy revs, remove dups, and stop after any revid in `backToRevs`:
                    vvl.sortLegacy();
                    slice lastRev;
                    auto  endBack = &backToRevs[backToRevsCount];
                    for ( auto i = vvl.legacy.begin(); i != vvl.legacy.end(); ) {
                        if ( *i == lastRev ) {
                            i = vvl.legacy.erase(i);  // remove duplicate
                        } else if ( std::find(backToRevs, endBack, *i) != endBack ) {
                            vvl.legacy.erase(i + 1, vvl.legacy.end());  // stop here
                            break;
                        } else {
                            lastRev = *i++;
                        }
                    }
                }

                // Finally convert to ASCII list.
                // Easter egg: if maxRevs is 0, don't replace '*' with my peer ID [tests use this]
                stringstream out;
                vvl.write(out, maxRevs ? mySourceID() : kMeSourceID);
                result = alloc_slice(out.str());
            }
            return result;
        }

        bool currentRevDescendsFrom(slice revID) const override {
            VersionVecWithLegacy localVec(_doc, RemoteID::Local);

            VersionVecWithLegacy ancestorVec([&] {
                if ( revidBuffer(revID).getRevID().isVersion() ) {
                    auto vec = VersionVector::fromASCII(revID);
                    return VersionVecWithLegacy(revid(vec.asBinary()));
                } else {
                    return VersionVecWithLegacy(&revID, 1, kMeSourceID);
                }
            }());

            auto cmp = VersionVecWithLegacy::compare(localVec, ancestorVec);
            return cmp == kNewer || cmp == kSame;
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
            if ( curRevID && curRevID.isVersion() ) return curRevID.asVersionVector();
            else
                return VersionVector();
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

        // Handles `c4doc_put` when `rq.existingRevision` is true (called by the Inserter during pull)
        int32_t putExistingRevision(const C4DocPutRequest& rq, C4Error* outError) override {
            VersionVecWithLegacy curVers(_doc, RemoteID::Local);
            VersionVecWithLegacy newVers((slice*)rq.history, rq.historyCount, mySourceID());
            auto                 remote = RemoteID(rq.remoteDBID);

            if ( !newVers.vector.updateClock(asInternal(database())->versionClock()) ) {
                if ( outError ) {
                    alloc_slice vecStr = newVers.vector.asASCII();
                    *outError          = c4error_printf(LiteCoreDomain, kC4ErrorBadRevisionID,
                                                        "Invalid timestamp in version vector %.*s", SPLAT(vecStr));
                }
                return -1;
            }

            Doc fldoc = _newProperties(rq, outError);
            if ( !fldoc ) return -1;
            Dict newProperties = fldoc.asDict();

            versionOrder order;
            int          commonAncestor;
            if ( newVers.vector.mergesSameVersions(curVers.vector) && newProperties.isEqual(_doc.properties()) ) {
                // This a redundant merge: same body, same MV.
                order = kSame;
                logPutExisting(curVers, newVers, order, remote);
                commonAncestor = 0;
                if ( remote == RemoteID::Local ) return commonAncestor;
                newProperties = _doc.properties();

            } else {
                // Compare it with the current document revision:
                order = VersionVecWithLegacy::compare(newVers, curVers);
                logPutExisting(curVers, newVers, order, remote);

                // Check for no-op or conflict:
                commonAncestor = 1;
                if ( order != kNewer ) {
                    if ( remote == RemoteID::Local ) {
                        if ( order == kConflicting ) {
                            c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                            return -1;
                        } else {
                            return 0;
                        }
                    }
                    if ( order != kConflicting ) commonAncestor = 0;
                }
            }

            alloc_slice newVersBinary;
            if ( newVers.vector.empty() ) newVersBinary = newVers.legacy.at(0);
            else
                newVersBinary = newVers.vector.asBinary();

            // Assemble a new Revision:
            Revision newRev;
            newRev.properties = newProperties;
            newRev.revID      = revid(newVersBinary);
            newRev.flags      = convertNewRevisionFlags(rq.revFlags);

            // Store the Revision into my VectorRecord:
            if ( order == kNewer ) {
                // It's newer, so update local to this revision:
                _doc.setCurrentRevision(newRev);
            } else if ( order == kConflicting ) {
                if ( !rq.allowConflict ) {
                    c4error_return(LiteCoreDomain, kC4ErrorConflict, nullslice, outError);
                    return -1;
                }
                // Conflict, so mark that and update only the remote:
                newRev.flags |= DocumentFlags::kConflicted;
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

        // Log the update. Normally verbose, but a conflict is info (if from the replicator)
        // or error (if local).
        void logPutExisting(VersionVecWithLegacy const& curVers, VersionVecWithLegacy const& newVers,
                            versionOrder order, RemoteID remote) {
            LogLevel level = LogLevel::Verbose;
            if ( order == kConflicting ) level = (remote == RemoteID::Local) ? LogLevel::Warning : LogLevel::Info;
            if ( DBLog.willLog(level) ) {
                static constexpr const char* kOrderName[4] = {"same", "older", "newer", "conflict"};
                stringstream                 out;
                out << "putExistingRevision '" << _docID << "' [" << newVers << "]; currently [" << curVers << "] --> "
                    << kOrderName[order] << " (remote " << int(remote) << ")";
                keyStore().dataFile()._log(level, "%s", out.str().c_str());
            }
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
            // Look up the Revisions:
            optional<pair<RemoteID, Revision>> won = _findRemote(winningRevID), lost = _findRemote(losingRevID);
            if ( !won || !lost ) error::_throw(error::NotFound, "Revision not found");
            if ( won->first == lost->first ) error::_throw(error::InvalidParameter, "That's the same revision");

            // One has to be Local, the other has to be remote and a conflict:
            Revision remoteRev;
            RemoteID remoteID;
            bool     localWon = won->first == RemoteID::Local;
            if ( localWon ) {
                tie(remoteID, remoteRev) = *lost;
            } else if ( lost->first == RemoteID::Local ) {
                tie(remoteID, remoteRev) = *won;
            } else {
                error::_throw(error::Conflict, "Conflict must involve the local revision");
            }
            if ( !(remoteRev.flags & DocumentFlags::kConflicted) )
                error::_throw(error::Conflict, "Revisions are not in conflict");

            // Parse mergedBody, but if it's equal to the winning rev's body, ignore it:
            Doc  mergedDoc;
            Dict mergedProperties;
            if ( mergedBody.buf ) {
                mergedDoc        = _newProperties(alloc_slice(mergedBody));
                mergedProperties = mergedDoc.asDict();
                if ( mergedProperties.isEqual(won->second.properties) ) {
                    mergedProperties = nullptr;
                    mergedBody       = nullslice;
                }
            }

            // Time to start the dance of the two revisions.  One or both could be legacy rev tree IDs at this point
            // and that needs to be accounted for.
            Revision      winningVersion = won->second;
            Revision      losingVersion  = lost->second;
            VersionVector mergedVersion;
            alloc_slice   mergedRevID;
            if ( mergedBody.buf ) {
                // In the case of a merge, we always create a resulting version vector
                if ( !winningVersion.hasVersionVector() && !losingVersion.hasVersionVector() ) {
                    // We can't use merge when both sides are legacy, because they will have the same
                    // fake author.  All we can do is just convert the winner.
                    mergedVersion.add(Version::legacyVersion(winningVersion.revID));
                } else {
                    // Otherwise, it's fair game to just throw everything into the merge function.  Any
                    // legacy rev IDs will be converted to the intermediate version vector form.
                    mergedVersion =
                            VersionVector::merge(toVersionVector(winningVersion), toVersionVector(losingVersion),
                                                 asInternal(database())->versionClock());
                }

                mergedRevID = mergedVersion.asBinary();
            } else if ( !winningVersion.hasVersionVector() || !losingVersion.hasVersionVector() ) {
                // At least one side had a legacy rev tree ID, so this requires some fuss.
                if ( localWon ) {
                    // Convert to a version vector up front, along the lines of "server branch switch"
                    // that we used to do when the local won in rev tree mode.
                    mergedVersion = VersionVector::trivialMerge(toVersionVector(winningVersion),
                                                                toVersionVector(losingVersion));
                    mergedRevID   = mergedVersion.asBinary();
                } else {
                    if ( losingVersion.hasVersionVector() ) {
                        // In this case, the rev tree ID of the remote won, but we already have a version
                        // vector saved.  We can't put the rev tree ID on top of it, so convert the rev
                        // tree ID to the intermediate version vector form.
                        mergedVersion = toVersionVector(winningVersion);
                        mergedRevID   = mergedVersion.asBinary();
                    } else {
                        // In this case the local ID is a rev tree ID, so it's safe to swap it out
                        // with the remote winning ID (which is either rev tree or version vector).
                        mergedRevID = winningVersion.revID;
                    }
                }
            } else {
                // Both sides are version vectors
                mergedVersion =
                        VersionVector::trivialMerge(toVersionVector(winningVersion), toVersionVector(losingVersion));
                mergedRevID = mergedVersion.asBinary();
            }

            // Update the local/current revision with the resulting merge:
            Revision mergedRev;
            mergedRev.revID = revid(mergedRevID);
            if ( mergedBody.buf ) {
                mergedRev.properties = mergedProperties;
                mergedRev.flags      = convertNewRevisionFlags(mergedFlags);
            } else {
                mergedRev.properties = won->second.properties;
                mergedRev.flags      = won->second.flags - DocumentFlags::kConflicted;
            }
            _doc.setCurrentRevision(mergedRev);

            // Remote rev is no longer a conflict:
            remoteRev.flags = remoteRev.flags - DocumentFlags::kConflicted;
            _doc.setRemoteRevision(remoteID, remoteRev);

            _updateDocFields();
            _selectRemote(RemoteID::Local);
            if ( !localWon ) std::swap(winningVersion, losingVersion);  // log local version first
            if ( DBLog.willLog(LogLevel::Info) ) {
                string winner = winningVersion.hasVersionVector() ? string(winningVersion.versionVector().asASCII())
                                                                  : winningVersion.revID.expanded().asString();
                string loser  = losingVersion.hasVersionVector() ? string(losingVersion.versionVector().asASCII())
                                                                 : losingVersion.revID.expanded().asString();
                LogTo(DBLog, "Resolved conflict in '%.*s' between #%s and #%s -> #%s", SPLAT(_docID), winner.c_str(),
                      loser.c_str(), mergedRev.revID.expanded().asString().c_str());
            }
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
        stringstream          result;
        VersionVector         localVec, requestedVec;
        optional<revidBuffer> requestedLegacyRev;

        auto compareLegacyToVector = [](revid legacyID, VersionVector const& vec) {
            if ( vec[0].author() == kLegacyRevSourceID ) {
                // Compare two tree revids:
                auto localTime  = Version::legacyVersion(legacyID).time();
                auto remoteTime = vec[0].time();
                if ( localTime < remoteTime ) return kOlder;
                else if ( localTime > remoteTime )
                    return kNewer;
                else
                    return kSame;
            } else {
                return kOlder;
            }
        };

        // Subroutine to compare a local version with the requested one:
        auto compareLocalRev = [&](revid localVersion) -> versionOrder {
            if ( requestedLegacyRev ) {
                // Request has a legacy revID:
                if ( localVersion.isVersion() ) {
                    // Local rev is a version vector:
                    localVec.readBinary(localVersion);
                    auto order = compareLegacyToVector(requestedLegacyRev->getRevID(), localVec);
                    return versionOrder(2 - order);  // reverse the order
                } else {
                    // Local rev is also a legacy revID:
                    auto cmp = localVersion.compare(requestedLegacyRev->getRevID());
                    if ( cmp < 0 ) return kOlder;
                    else if ( cmp > 0 )
                        return kNewer;
                    else
                        return kSame;
                }
            } else {
                // Request has a version vector, requestedVec:
                if ( localVersion.isVersion() ) {
                    // Local rev also has a version vector:
                    localVec.readBinary(localVersion);
                    return localVec.compareTo(requestedVec);
                } else {
                    // Local rev is a legacy revid:
                    return compareLegacyToVector(localVersion, requestedVec);
                }
            }
        };

        auto callback = [&](const RecordUpdate& rec) -> alloc_slice {
            // --- This callback runs inside the SQLite query ---
            // --- It will be called once for each existing requested docID, in arbitrary order ---

            // Look up matching requested revID, and convert to encoded binary form:
            slice rev            = revMap[rec.key];
            bool  requestUsesVVs = rev.findByte('@') != nullptr;
            if ( requestUsesVVs ) {
                requestedVec.readASCII(rev, mySourceID);
                requestedLegacyRev.reset();
            } else {
                requestedVec.clear();
                requestedLegacyRev.emplace();
                requestedLegacyRev->parse(rev);
            }

            // Check whether the doc's current rev is this version, or a newer, or a conflict:
            versionOrder cmp;
            cmp         = compareLocalRev(revid(rec.version));
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

            // I don't have the requested rev, so find all my current revs
            // and append them as a JSON array:
            result.str("");
            result << statusChar << '[';

            std::set<alloc_slice> added;
            delimiter             delim(",");
            VectorRecord::forAllRevIDs(rec, [&](RemoteID, revid aRev, bool hasBody) {
                if ( delim.count() < maxAncestors && hasBody >= mustHaveBodies ) {
                    alloc_slice vector;
                    if ( aRev.isVersion() ) {
                        vector = aRev.asVersion().asASCII(mySourceID);
                    } else {
                        vector = aRev.expanded();
                    }
                    if ( vector && added.insert(vector).second )  // [skip duplicate vectors]
                        result << delim << '"' << vector << '"';
                }
            });

            result << ']';
            return alloc_slice(result.str());  // --> Done!
        };
        return asInternal(collection())->keyStore().withDocBodies(docIDs, callback);
    }


}  // namespace litecore
