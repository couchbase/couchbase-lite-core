//
// RevFinder.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "RevFinder.hh"
#include "Replicator.hh"
#include "Pusher.hh"
#include "ReplicatorTuning.hh"
#include "DBAccess.hh"
#include "Increment.hh"
#include "VersionVector.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include "fleece/Fleece.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore::repl {

    RevFinder::RevFinder(Replicator* replicator, Delegate* delegate, CollectionIndex coll)
        : Worker(replicator, "RevFinder", coll), _delegate(delegate) {
        setParentObjectRef(replicator->getObjectRef());
#ifdef LITECORE_CPPTEST
        _disableReplacementRevs = replicator->_disableReplacementRevs;
#endif
        _mustBeProposed = passive() && _options->noIncomingConflicts() && !_db->usingVersionVectors();
        replicator->registerWorkerHandler(this, "changes", &RevFinder::handleChanges);
        replicator->registerWorkerHandler(this, "proposeChanges", &RevFinder::handleChanges);
    }

    void RevFinder::onError(C4Error err) {
        // If the database closes on replication stop, this error might happen
        // but it is inconsequential so suppress it.  It will still be logged, but
        // not in the worker's error property.
        if ( err.domain != LiteCoreDomain || err.code != kC4ErrorNotOpen ) { Worker::onError(err); }
    }

    // Receiving an incoming "changes" (or "proposeChanges") message
    void RevFinder::handleChanges(Retained<MessageIn> req) {
        if ( pullerHasCapacity() ) {
            handleChangesNow(req);
        } else {
            logVerbose("Queued '%.*s' REQ#%" PRIu64 " (now %zu)", SPLAT(req->property("Profile"_sl)), req->number(),
                       _waitingChangesMessages.size() + 1);
            Signpost::begin(Signpost::handlingChanges, (uintptr_t)req->number());
            _waitingChangesMessages.push_back(std::move(req));
        }
    }

    void RevFinder::_reRequestingRev() { increment(_numRevsBeingRequested); }

    void RevFinder::_revReceived() {
        decrement(_numRevsBeingRequested);

        // Process waiting "changes" messages if not throttled:
        while ( !_waitingChangesMessages.empty() && pullerHasCapacity() ) {
            auto req = _waitingChangesMessages.front();
            _waitingChangesMessages.pop_front();
            handleChangesNow(req);
        }
    }

    void RevFinder::_revokedHandled(unsigned count) {
        decrement(_numRevokedBeingHandled, count);

        // Process waiting "changes" messages if not throttled:
        while ( !_waitingChangesMessages.empty() && pullerHasCapacity() ) {
            auto req = _waitingChangesMessages.front();
            _waitingChangesMessages.pop_front();
            handleChangesNow(req);
        }
    }

    // Actually handle a "changes" (or "proposeChanges") message:
    void RevFinder::handleChangesNow(MessageIn* req) {
        try {
            slice reqType  = req->property("Profile"_sl);
            bool  proposed = (reqType == "proposeChanges"_sl);
            logVerbose("Handling '%.*s' REQ#%" PRIu64, SPLAT(reqType), req->number());

            auto changes  = req->JSONBody().asArray();
            auto nChanges = changes.count();
            if ( !changes && req->body() != "null"_sl ) {
                warn("Invalid body of 'changes' message");
                req->respondWithError({"BLIP"_sl, 400, "Invalid JSON body"_sl});
            } else if ( !proposed && _mustBeProposed ) {
                // In conflict-free mode plus rev-trees the protocol requires the pusher send
                // "proposeChanges" instead.
                req->respondWithError({"BLIP"_sl, 409});
            } else if ( nChanges == 0 ) {
                // Empty array indicates we've caught up. (This may have been sent no-reply)
                logInfo("Caught up with remote changes");
                _delegate->caughtUp();
                req->respond();
            } else if ( req->noReply() ) {
                warn("Got pointless noreply 'changes' message");
            } else {
                // Alright, let's look at the changes:
                if ( proposed ) {
                    logInfo("Received %u changes", nChanges);
                } else if ( willLog() ) {
                    alloc_slice firstSeq(changes[0].asArray()[0].toString());
                    alloc_slice lastSeq(changes.get(nChanges - 1).asArray()[0].toString());
                    logInfo("Received %u changes (seq '%.*s'..'%.*s')", nChanges, SPLAT(firstSeq), SPLAT(lastSeq));
                }

                if ( !proposed ) _db->markRevsSyncedNow();  // make sure foreign ancestors are up to date

                MessageBuilder response(req);
                response.compressed = true;
                if ( !_db->usingVersionVectors() ) {
                    // Depth of rev history SG should send to us
                    response["maxHistory"_sl] = tuning::kDefaultMaxHistory;
                }
                if ( !_db->disableBlobSupport() ) response["blobs"_sl] = "true"_sl;
                if ( !_announcedDeltaSupport && !_options->disableDeltaSupport() ) {
                    response["deltas"_sl]  = "true"_sl;
                    _announcedDeltaSupport = true;
                }

#ifdef LITECORE_CPPTEST
                response["sendReplacementRevs"] = !_disableReplacementRevs;
#else
                response["sendReplacementRevs"] = tuning::kChangesReplacementRevs;
#endif

                Stopwatch st;

                vector<ChangeSequence> sequences;  // the vector I will send to the delegate
                sequences.reserve(nChanges);

                auto& encoder           = response.jsonBody();
                auto  getConflictRevIDs = req->boolProperty(Pusher::kConflictIncludesRevProperty);
                encoder.beginArray();
                unsigned requested = proposed ? findProposedRevs(changes, encoder, getConflictRevIDs, sequences)
                                              : findRevs(changes, encoder, sequences);
                encoder.endArray();

                // CBL-1399: Important that the order be call expectSequences and *then* respond
                // to avoid rev messages comes in before the Puller knows about them (mostly
                // applies to local to local replication where things can come back over the wire
                // very quickly)
                _numRevsBeingRequested += requested;
                _delegate->expectSequences(std::move(sequences));
                req->respond(response);

                logInfo("Responded to '%.*s' REQ#%" PRIu64 " w/request for %u revs in %.6f sec",
                        SPLAT(req->property("Profile"_sl)), req->number(), requested, st.elapsed());
            }
        } catch ( ... ) {
            auto error = C4Error::fromCurrentException();
            gotError(error);
            req->respondWithError(c4ToBLIPError(error));
        }
        Signpost::end(Signpost::handlingChanges, (uintptr_t)req->number());
    }

    void RevFinder::checkDocAndRevID(slice docID, slice revID) {
        bool valid;
        if ( docID.size < 1 || docID.size > 255 ) valid = false;
        else if ( _db->usingVersionVectors() )
            valid = revID.findByte('@') && !revID.findByte('*');  // require absolute form
        else
            valid = revID.findByte('-');
        if ( !valid ) {
            C4Error::raise(LiteCoreDomain, kC4ErrorRemoteError,
                           "Invalid docID/revID '%.*s' #%.*s in incoming change list", SPLAT(docID), SPLAT(revID));
        }
    }

    // Looks through the contents of a "changes" message, encodes the response,
    // adds each entry to `sequences`, and returns the number of new revs.
    unsigned RevFinder::findRevs(Array changes, JSONEncoder& encoder, vector<ChangeSequence>& sequences) {
        // Compile the docIDs/revIDs into parallel vectors:
        vector<slice>                 docIDs, revIDs;
        vector<Retained<RevToInsert>> revoked;
        vector<uint32_t>              changeIndexes;
        auto                          nChanges = changes.count();
        docIDs.reserve(nChanges);
        revIDs.reserve(nChanges);
        uint32_t changeIndex = 0;
        for ( auto item : changes ) {
            // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
            auto     change   = item.asArray();
            slice    docID    = change[1].asString();
            slice    revID    = change[2].asString();
            int64_t  deletion = change[3].asInt();
            uint64_t bodySize = change[4].asUnsigned();

            // Validate docID and revID:
            checkDocAndRevID(docID, revID);

            if ( deletion <= 1 || deletion == 0b101 ) {
                // New revision or tombstone or (tombstone+removal):
                // The removal flag, 0b100, could be due to pushing a tombstone to the SGW.
                // We disregard the removal flag in this case.
                docIDs.push_back(docID);
                revIDs.push_back(revID);
                changeIndexes.push_back(changeIndex);
                sequences.push_back({RemoteSequence(change[0]), max(bodySize, (uint64_t)1)});
            } else {
                // Access lost -- doc removed from channel, or user lost access to channel.
                // In SG 2.x "deletion" is a boolean flag, 0=normal, 1=deleted.
                // SG 3.x adds 2=revoked, 3=revoked+deleted, 4=removal (from channel)
                // If the removal flag is accompanyied by the deleted flag, we don't purge, c.f. above remark.
                auto mode = (deletion < 4) ? RevocationMode::kRevokedAccess : RevocationMode::kRemovedFromChannel;
                logInfo("SG revoked access to rev \"%.*s.%.*s.%.*s/%.*s\" with deletion %lld",
                        SPLAT(collectionSpec().scope), SPLAT(collectionSpec().name), SPLAT(docID), SPLAT(revID),
                        deletion);
                revoked.emplace_back(new RevToInsert(docID, revID, mode, collectionSpec(),
                                                     _options->collectionCallbackContext(collectionIndex())));
                sequences.push_back({RemoteSequence(change[0]), 0});
            }
            ++changeIndex;
        }

        if ( !revoked.empty() ) {
            increment(_numRevokedBeingHandled, (unsigned)revoked.size());
            _delegate->documentsRevoked(std::move(revoked));
        }

        // Ask the database to look up the ancestors:
        vector<alloc_slice> ancestors = _db->useCollection(collectionSpec())
                                                ->findDocAncestors(docIDs, revIDs, kMaxPossibleAncestors,
                                                                   !_options->disableDeltaSupport(),  // requireBodies
                                                                   _db->remoteDBID());
        // Look through the database response:
        unsigned itemsWritten = 0, requested = 0;
        for ( unsigned i = 0; i < changeIndexes.size(); ++i ) {
            slice                         docID = docIDs[i], revID = revIDs[i];
            alloc_slice                   anc(std::move(ancestors[i]));
            C4FindDocAncestorsResultFlags status = anc ? (anc[0] - '0') : kRevsLocalIsOlder;

            if ( status & kRevsLocalIsOlder ) {
                // I have an older revision or a conflict:
                // First, append zeros for any items I skipped:
                // [use only writeRaw to avoid confusing JSONEncoder's comma mechanism, CBL-1208]
                if ( itemsWritten > 0 ) encoder.writeRaw(",");  // comma after previous array item
                while ( itemsWritten++ < changeIndexes[i] ) encoder.writeRaw("0,");

                if ( (status & kRevsConflict) == kRevsConflict && passive() ) {
                    // Passive puller refuses conflicts:
                    encoder.writeRaw("409");
                    sequences[changeIndexes[i]].bodySize = 0;
                    logDebug("    - '%.*s' #%.*s conflicts with local revision, rejecting", SPLAT(docID), SPLAT(revID));
                } else {
                    // OK, I want it!
                    // Append array of ancestor revs I do have (it's already a JSON array):
                    ++requested;
                    slice jsonArray = (anc ? anc.from(1) : "[]"_sl);
                    encoder.writeRaw(jsonArray);
                    logDebug("    - Requesting '%.*s' #%.*s, I have ancestors %.*s", SPLAT(docID), SPLAT(revID),
                             SPLAT(jsonArray));
                }
            } else {
                // I have an equal or newer revision; ignore this one:
                // [Implicitly this appends a 0, but we're skipping trailing zeroes.]
                sequences[changeIndexes[i]].bodySize = 0;
                if ( status & kRevsAtThisRemote ) {
                    logDebug("    - Already have '%.*s' %.*s", SPLAT(docID), SPLAT(revID));
                } else {
                    // This means the rev exists but is not marked as the latest from the
                    // remote server, so I better make it so:
                    logDebug("    - Already have '%.*s' %.*s but need to mark it as remote ancestor", SPLAT(docID),
                             SPLAT(revID));
                    _db->setDocRemoteAncestor(collectionSpec(), docID, revID);
                    if ( !passive() && !_db->usingVersionVectors() ) {
                        auto repl = replicatorIfAny();
                        if ( repl ) {
                            repl->docRemoteAncestorChanged(alloc_slice(docID), alloc_slice(revID), collectionIndex());
                        } else {
                            Warn("findRevs no longer has a replicator reference (replicator stopped?), "
                                 "ignoring docRemoteAncestorChange callback");
                        }
                    }
                }
            }
        }
        return requested;
    }

    // Same as `findOrRequestRevs`, but for "proposeChanges" messages.
    unsigned RevFinder::findProposedRevs(Array changes, JSONEncoder& encoder, bool conflictIncludesRev,
                                         vector<ChangeSequence>& sequences) {
        unsigned itemsWritten = 0, requested = 0;
        int      i = -1;
        for ( auto item : changes ) {
            ++i;
            // Look up each revision in the `req` list:
            // "proposeChanges" entry: [docID, revID, parentRevID?, bodySize?]
            auto        change = item.asArray();
            alloc_slice docID(change[0].asString());
            slice       revID = change[1].asString();
            checkDocAndRevID(docID, revID);

            slice parentRevID = change[2].asString();
            if ( parentRevID.size == 0 ) parentRevID = nullslice;
            alloc_slice currentRevID;
            int         status = findProposedChange(docID, revID, parentRevID, currentRevID);
            if ( status == 0 ) {
                // Accept rev by (lazily) appending a 0:
                logDebug("    - Accepting proposed change '%.*s' #%.*s with parent %.*s", SPLAT(docID), SPLAT(revID),
                         SPLAT(parentRevID));
                ++requested;
                sequences.push_back({RemoteSequence(), max(change[3].asUnsigned(), (uint64_t)1)});
                // sequences[i].sequence remains null: proposeChanges entries have no sequence ID
            } else {
                // Reject rev by appending status code:
                logInfo("Rejecting proposed change '%.*s' #%.*s with parent %.*s (status %d; current rev is %.*s)",
                        SPLAT(docID), SPLAT(revID), SPLAT(parentRevID), status, SPLAT(currentRevID));
                while ( itemsWritten++ < i ) encoder.writeInt(0);

                if ( status == 409 && conflictIncludesRev ) {
                    encoder.beginDict(2);
                    encoder.writeKey("status"_sl);
                    encoder.writeInt(409);
                    encoder.writeKey("rev"_sl);
                    encoder.writeString(currentRevID);
                    encoder.endDict();
                } else {
                    encoder.writeInt(status);
                }
            }
        }
        return requested;
    }

    // Checks whether the revID (if any) is really current for the given doc.
    // Returns an HTTP-ish status code: 0=OK, 409=conflict, 500=internal error
    int RevFinder::findProposedChange(slice docID, slice revID, slice parentRevID, alloc_slice& outCurrentRevID) {
        C4DocumentFlags flags = 0;
        {
            // Get the local doc's current revID/vector and flags:
            outCurrentRevID = nullslice;
            try {
                if ( Retained<C4Document> doc = _db->getDoc(collectionSpec(), docID, kDocGetMetadata); doc ) {
                    flags           = doc->flags();
                    outCurrentRevID = doc->getSelectedRevIDGlobalForm();
                }
            } catch ( ... ) {
                gotError(C4Error::fromCurrentException());
                return 500;
            }
        }

        if ( outCurrentRevID == revID ) {
            // I already have this revision:
            return 304;
        } else if ( _db->usingVersionVectors() ) {
            // Version vectors:  (note that parentRevID is ignored; we don't need it)
            try {
                auto theirVers = VersionVector::fromASCII(revID);
                auto myVers    = VersionVector::fromASCII(outCurrentRevID);
                switch ( theirVers.compareTo(myVers) ) {
                    case kSame:
                    case kOlder:
                        return 304;
                    case kNewer:
                        return 0;
                    case kConflicting:
                        return 409;
                }
                abort();  // unreachable
            } catch ( const error& x ) {
                if ( x == error::BadRevisionID ) return 500;
                else
                    throw;
            }
        } else {
            // Rev-trees:
            // I don't have this revision and it's not a conflict, so I want it!
            if ( outCurrentRevID == parentRevID ||
                 // Peer is creating a new doc; my doc is deleted, so that's OK
                 (!parentRevID && (flags & kDocDeleted)) )
                return 0;
            else
                return 409;  // Peer's revID isn't current, so this is a conflict
        }
    }


}  // namespace litecore::repl
