//
// Pusher+Revs.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Pusher.hh"
#include "PropertyEncryption.hh"
#include "DBAccess.hh"
#include "ReplicatorTuning.hh"
#include "HTTPTypes.hh"
#include "Increment.hh"
#include "StringUtil.hh"
#include "fleece/Mutable.hh"
#include <cinttypes>

using namespace std;
using namespace litecore::blip;

namespace litecore::repl {

    void Pusher::maybeSendMoreRevs() {
        while ( _revisionsInFlight < tuning::kMaxRevsInFlight
                && _revisionBytesAwaitingReply <= tuning::kMaxRevBytesAwaitingReply && !_revQueue.empty() ) {
            Retained<RevToSend> first = std::move(_revQueue.front());
            _revQueue.pop_front();
            sendRevision(first);
            if ( _revQueue.size() == tuning::kMaxRevsQueued - 1 )
                maybeGetMoreChanges();  // I may now be eligible to send more changes
        }
        //        if (!_revQueue.empty())
        //            logVerbose("Throttling sending revs; _revisionsInFlight=%u/%u, _revisionBytesAwaitingReply=%llu/%u",
        //                       _revisionsInFlight, tuning::kMaxRevsInFlight,
        //                       _revisionBytesAwaitingReply, tuning::kMaxRevBytesAwaitingReply);
    }

    // Send a "rev" message containing a revision body.
    void Pusher::sendRevision(Retained<RevToSend> request) {
        if ( !connected() ) return;

        logVerbose("Sending rev '%.*s' #%.*s (seq #%" PRIu64 ") [%d/%d]", SPLAT(request->docID), SPLAT(request->revID),
                   (uint64_t)request->sequence, _revisionsInFlight, tuning::kMaxRevsInFlight);

        // Get the document & revision:
        C4Error              c4err = {};
        Dict                 root;
        auto                 collection       = getCollection();
        slice                replacementRevID = nullslice;
        Retained<C4Document> doc = _db->useCollection(collection)->getDocument(request->docID, true, kDocGetUpgraded);
        if ( doc ) {
            if ( doc->selectRevision(request->revID, true) ) root = doc->getProperties();
            if ( root ) request->flags = doc->selectedRev().flags;
            else if ( _sendReplacementRevs && doc->selectCurrentRevision() && doc->loadRevisionBody() ) {
                root = doc->getProperties();
                if ( root ) {
                    request->flags   = doc->selectedRev().flags;
                    replacementRevID = doc->selectedRev().revID;
                } else
                    revToSendIsObsolete(*request, &c4err);
            } else {
                revToSendIsObsolete(*request, &c4err);
            }
        } else {
            c4err = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
        }

        // In general, this method won't call doneWithRev(), unless we do not have the
        // body of the rev to send (when root is null). In this case, we send an error
        // to the remote and call doneWithRev() with argument 'completed' set to false.
        // The one exception is when the Encyptor callback returns an error, when we will
        // mark the rev is "permanently" completed and set 'completed' to true.
        bool completed = false;

        // Encrypt any encryptable properties
        MutableDict encryptedRoot;
        if ( root && MayContainPropertiesToEncrypt(doc->getRevisionBody()) ) {
            logVerbose("Encrypting properties in doc '%.*s'", SPLAT(request->docID));
            encryptedRoot = EncryptDocumentProperties(request->collectionSpec, request->docID, root,
                                                      _options->propertyEncryptor, _options->callbackContext, &c4err);
            if ( encryptedRoot ) {
                root = encryptedRoot;
            } else {
                // Error: we don't get the encrypted body.
                // If the encryptor has not specified an error, we assign
                // the following error.
                if ( !c4err ) { c4err = {LiteCoreDomain, kC4ErrorCrypto}; }
                finishedDocumentWithError(request, c4err, false);

                if ( c4err.domain == WebSocketDomain && c4err.code == 503 ) {
                    // This is treated as a transient network glitch, we lift it to the replicator
                    // to handle. The replicator will be taken to offline and restarted after a certain
                    // wait time.
                    onError(c4err);
                    return;
                }

                root = nullptr;
                // Encyptor error is permanent.
                completed = true;
            }
        }

        auto fullRevID = alloc_slice(_db->convertVersionToAbsolute(request->revID));
        auto fullReplacementRevID =
                replacementRevID ? alloc_slice(_db->convertVersionToAbsolute(replacementRevID)) : nullslice;

        // Now send the BLIP message. Normally it's "rev", but if this is an error we make it
        // "norev" and include the error code:
        MessageBuilder msg(root ? "rev"_sl : "norev"_sl);
        assignCollectionToMsg(msg, collectionIndex());
        msg.compressed = true;
        msg["id"_sl]   = request->docID;
        if ( fullReplacementRevID ) {
            msg["rev"_sl]      = fullReplacementRevID;
            msg["replacedRev"] = fullRevID;
        } else
            msg["rev"_sl] = fullRevID;
        msg["sequence"_sl] = narrow_cast<int64_t>((uint64_t)request->sequence);
        if ( root ) {
            if ( request->noConflicts ) msg["noconflicts"_sl] = true;
            auto revisionFlags = doc->selectedRev().flags;
            if ( revisionFlags & kRevDeleted ) msg["deleted"_sl] = "1"_sl;

            // Include the document history, but skip the current revision 'cause it's redundant
            alloc_slice history = request->historyString(doc);
            alloc_slice effectiveRevID = fullReplacementRevID ? fullReplacementRevID : fullRevID;
            if ( history.hasPrefix(effectiveRevID) && history.size > effectiveRevID.size )
                msg["history"_sl] = history.from(effectiveRevID.size + 1);

            bool sendLegacyAttachments =
                    (request->legacyAttachments && (revisionFlags & kRevHasAttachments) && !_db->disableBlobSupport());

            // Delta compression (unless we encrypted properties):
            alloc_slice deltaJSON;
            if ( !encryptedRoot ) {
                deltaJSON = createRevisionDelta(doc, request, root, doc->getRevisionBody().size, sendLegacyAttachments);
            }
            if ( deltaJSON ) {
                msg["deltaSrc"_sl] = _db->convertVersionToAbsolute(doc->selectedRev().revID);
                msg.jsonBody().writeRaw(deltaJSON);
            } else if ( root.empty() ) {
                msg.write("{}"_sl);
            } else {
                auto& bodyEncoder = msg.jsonBody();
                if ( sendLegacyAttachments ) {
                    unsigned revpos = 0;
                    if ( !_db->usingVersionVectors() ) revpos = C4Document::getRevIDGeneration(request->revID);
                    _db->encodeRevWithLegacyAttachments(bodyEncoder, root, revpos);
                } else {
                    bodyEncoder.writeValue(root);
                }
            }
            logVerbose("Transmitting 'rev' message with '%.*s' #%.*s", SPLAT(request->docID), SPLAT(request->revID));
            sendRequest(msg, [this, request](const MessageProgress& progress) { onRevProgress(request, progress); });
            increment(_revisionsInFlight);

        } else {
            // Send an error if we couldn't get the revision:
            int blipError;
            if ( c4err.domain == WebSocketDomain ) blipError = c4err.code;
            else if ( c4err.domain == LiteCoreDomain && c4err.code == kC4ErrorNotFound )
                blipError = 404;
            else {
                warn("sendRevision: Couldn't get rev '%.*s' %.*s from db: %s", SPLAT(request->docID),
                     SPLAT(request->revID), c4err.description().c_str());
                blipError = 500;
            }
            msg["error"_sl] = blipError;
            msg.noreply     = true;
            sendRequest(msg);

            doneWithRev(request, completed, false);
            enqueue(FUNCTION_TO_QUEUE(Pusher::maybeSendMoreRevs));  // async call to avoid recursion
        }
    }

    // "rev" message progress callback:
    void Pusher::onRevProgress(const Retained<RevToSend>& rev, const MessageProgress& progress) {
        switch ( progress.state ) {
            case MessageProgress::kDisconnected:
                doneWithRev(rev, false, false);
                break;
            case MessageProgress::kAwaitingReply:
                logDebug("Transmitted 'rev' %.*s #%.*s (seq #%" PRIu64 ")", SPLAT(rev->docID), SPLAT(rev->revID),
                         static_cast<uint64_t>(rev->sequence));
                decrement(_revisionsInFlight);
                increment(_revisionBytesAwaitingReply, progress.bytesSent);
                maybeSendMoreRevs();
                break;
            case MessageProgress::kComplete:
                {
                    decrement(_revisionBytesAwaitingReply, progress.bytesSent);
                    bool synced    = !progress.reply->isError();
                    bool completed = true;

                    enum { kNoRetry, kRetryLater, kRetryNow } retry = kNoRetry;

                    if ( synced ) {
                        logVerbose("Completed rev %.*s #%.*s (seq #%" PRIu64 ")", SPLAT(rev->docID), SPLAT(rev->revID),
                                   (uint64_t)rev->sequence);
                        finishedDocument(rev);
                    } else {
                        // Handle an error received from the peer:
                        auto err   = progress.reply->getError();
                        auto c4err = blipToC4Error(err);

                        if ( c4err.mayBeTransient() ) {
                            completed = false;
                        } else if ( c4err == C4Error{WebSocketDomain, 403} ) {
                            // CBL-123: Retry HTTP forbidden once
                            if ( rev->retryCount++ == 0 ) {
                                completed = false;
                                if ( !passive() ) retry = kRetryLater;
                            }
                        } else if ( c4err == C4Error{LiteCoreDomain, kC4ErrorDeltaBaseUnknown}
                                    || c4err == C4Error{LiteCoreDomain, kC4ErrorCorruptDelta}
                                    || c4err == C4Error{WebSocketDomain, int(net::HTTPStatus::UnprocessableEntity)} ) {
                            // CBL-986: On delta error, retry without using delta
                            if ( rev->deltaOK ) {
                                rev->deltaOK = false;
                                completed    = false;
                                retry        = kRetryNow;
                            }
                        }

                        logError("Got %-serror response to rev '%.*s' #%.*s (seq #%" PRIu64 "): %.*s %d '%.*s'",
                                 (completed ? "" : "transient "), SPLAT(rev->docID), SPLAT(rev->revID),
                                 (uint64_t)rev->sequence, SPLAT(err.domain), err.code, SPLAT(err.message));
                        if ( completed && c4err.code == 403 ) {
                            rev->rejectedByRemote = true;
                            _db->markRevSynced(rev);
                        }
                        // It's safe to not call finishedDocumentWithError if we are going to retry it
                        // immediately. In this case, we don't put it into _docsEnded now. It wil be
                        // taken care of after retry.
                        //
                        if ( retry != kRetryNow ) finishedDocumentWithError(rev, c4err, !completed);

                        // If this is a permanent failure, like a validation error or conflict,
                        // then I've completed my duty to push it.
                    }
                    doneWithRev(rev, completed, synced);
                    switch ( retry ) {
                        case kRetryNow:
                            retryRevs({rev}, true);
                            break;
                        case kRetryLater:
                            _revsToRetry.push_back(rev);
                            break;
                        case kNoRetry:
                            break;
                    }
                    maybeSendMoreRevs();
                    break;
                }
            default:
                break;
        }
    }

    // If sending a rev that's been obsoleted by a newer one, mark the sequence as complete and send
    // a 410 Gone error. (Common subroutine of sendRevision & shouldRetryConflictWithNewerAncestor.)
    void Pusher::revToSendIsObsolete(const RevToSend& request, C4Error* c4err) {
        logInfo("Revision '%.*s' #%.*s is obsolete; not sending it", SPLAT(request.docID), SPLAT(request.revID));
        if ( !passive() ) _checkpointer.completedSequence(request.sequence);
        if ( c4err ) *c4err = {WebSocketDomain, 410};  // Gone
    }

    // Attempt to delta-compress the revision; returns JSON delta or a null slice.
    alloc_slice Pusher::createRevisionDelta(C4Document* doc, RevToSend* request, Dict root, size_t revisionSize,
                                            bool sendLegacyAttachments) {
        alloc_slice delta;
        if ( !request->deltaOK || revisionSize < tuning::kMinBodySizeForDelta || _options->disableDeltaSupport() )
            return delta;

        // Find an ancestor revision known to the server:
        C4RevisionFlags ancestorFlags = 0;
        Dict            ancestor;
        slice           ancestorRevID;
        if ( request->remoteAncestorRevID && doc->selectRevision(request->remoteAncestorRevID, true) ) {
            ancestor      = doc->getProperties();
            ancestorFlags = doc->selectedRev().flags;
            ancestorRevID = doc->selectedRev().revID;
        }

        if ( ancestorFlags & kRevDeleted ) return delta;

        if ( !ancestor && request->ancestorRevIDs ) {
            for ( const auto& revID : *request->ancestorRevIDs ) {
                if ( doc->selectRevision(revID, true) ) {
                    ancestor      = doc->getProperties();
                    ancestorFlags = doc->selectedRev().flags;
                    ancestorRevID = doc->selectedRev().revID;
                    break;
                }
            }
        }
        if ( ancestor.empty() ) return delta;

        Doc legacyOld, legacyNew;
        if ( sendLegacyAttachments ) {
            // If server needs legacy attachment layout, transform the bodies:
            Encoder  enc;
            unsigned revPos = 0;
            if ( !_db->usingVersionVectors() ) revPos = C4Document::getRevIDGeneration(request->revID);
            _db->encodeRevWithLegacyAttachments(enc, root, revPos);
            legacyNew = enc.finishDoc();
            root      = legacyNew.root().asDict();

            if ( ancestorFlags & kRevHasAttachments ) {
                enc.reset();
                // Use revpos from the ancester's revID
                if ( !_db->usingVersionVectors() ) revPos = C4Document::getRevIDGeneration(ancestorRevID);
                _db->encodeRevWithLegacyAttachments(enc, ancestor, revPos);
                legacyOld = enc.finishDoc();
                ancestor  = legacyOld.root().asDict();
            }
        }

        delta = FLCreateJSONDelta(ancestor, root);
        if ( !delta || narrow_cast<double>(delta.size) > narrow_cast<double>(revisionSize) * 1.2 )
            return {};  // Delta failed, or is (probably) bigger than body; don't use

        if ( willLog(LogLevel::Verbose) ) {
            alloc_slice old(ancestor.toJSON());
            alloc_slice nuu(root.toJSON());
            logVerbose("Encoded revision as delta, saving %zd bytes:\n\told = %.*s\n\tnew = %.*s\n\tDelta = %.*s",
                       nuu.size - delta.size, SPLAT(old), SPLAT(nuu), SPLAT(delta));
        }
#ifdef LITECORE_CPPTEST
        slice cbl_4499_errDoc = "cbl-4499_doc-001"_sl;
        if ( doc->docID().hasSuffix(cbl_4499_errDoc) ) {
            string s  = delta.asString();
            auto   p0 = s.find(':');
            auto   p1 = s.find(',');
            delta     = alloc_slice(s.substr(0, p0 + 1) + "[\"xyz\", 0, 10]" + s.substr(p1));
        }
#endif
        return delta;
    }

    // Finished sending a revision (successfully or not.)
    // `completed` - whether to mark the sequence as completed in the checkpointer
    // `synced` - whether the revision was successfully stored on the peer
    void Pusher::doneWithRev(RevToSend* rev, bool completed, bool synced) {
        if ( !passive() ) {
            logDebug("** doneWithRev %.*s #%.*s", SPLAT(rev->docID), SPLAT(rev->revID));  //TEMP
            addProgress({rev->bodySize, 0});
            if ( completed ) {
                _checkpointer.completedSequence(rev->sequence);

                auto lastSeq = _checkpointer.localMinSequence();
                if ( uint64_t(lastSeq) / 1000 > uint64_t(_lastSequenceLogged) / 1000 || willLog(LogLevel::Verbose) )
                    logInfo("Checkpoint now %s", _checkpointer.to_string().c_str());
                _lastSequenceLogged = lastSeq;
            }
            if ( synced ) _db->markRevSynced(const_cast<RevToSend*>(rev));
        }

        // Remove rev from _pushingDocs, and see if there's a newer revision to send next:
        Retained<RevToSend> newRev = std::move(rev->nextRev);
        _pushingDocs.erase(rev->docID);
        if ( newRev ) {
            if ( synced && getForeignAncestors() ) newRev->remoteAncestorRevID = rev->revID;
            logVerbose("Now that '%.*s' %.*s is done, propose %.*s (remote %.*s) ...", SPLAT(rev->docID),
                       SPLAT(rev->revID), SPLAT(newRev->revID), SPLAT(newRev->remoteAncestorRevID));
            bool ok = false;
            if ( synced && getForeignAncestors() && !_db->usingVersionVectors()
                 && C4Document::getRevIDGeneration(newRev->revID) <= C4Document::getRevIDGeneration(rev->revID) ) {
                // Don't send; it'll conflict with what's on the server
            } else {
                // Send newRev as though it had just arrived:
                if ( _changesFeed.shouldPushRev(newRev) ) {
                    gotOutOfOrderChange(newRev);
                    ok = true;
                }
            }
            if ( !ok ) {
                logVerbose("   ... nope, decided not to propose '%.*s' %.*s", SPLAT(newRev->docID),
                           SPLAT(newRev->revID));
                _checkpointer.completedSequence(newRev->sequence);
            }
        } else {
            logDebug("Done pushing '%.*s' %.*s", SPLAT(rev->docID), SPLAT(rev->revID));
        }
    }

}  // namespace litecore::repl
