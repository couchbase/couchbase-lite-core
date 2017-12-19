//
//  DBWorker.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/21/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DBWorker.hh"
#include "Pusher.hh"
#include "IncomingRev.hh"
#include "FleeceCpp.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "Stopwatch.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "c4Private.h"
#include "BLIP.hh"
#include <chrono>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static constexpr slice kLocalCheckpointStore = "checkpoints"_sl;
    static constexpr slice kPeerCheckpointStore  = "peerCheckpoints"_sl;

    static constexpr auto kInsertionDelay = chrono::milliseconds(50);

    static constexpr size_t kMinBodySizeToCompress = 500;
    

    static bool isNotFoundError(C4Error err) {
        return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
    }

    static bool hasConflict(C4Document *doc) {
        return c4doc_selectCurrentRevision(doc)
            && c4doc_selectNextLeafRevision(doc, false, false, nullptr);
    }


    DBWorker::DBWorker(Connection *connection,
                     Replicator *replicator,
                     C4Database *db,
                     const websocket::Address &remoteAddress,
                     Options options)
    :Worker(connection, replicator, options, "DB")
    ,_db(c4db_retain(db))
    ,_blobStore(c4db_getBlobStore(db, nullptr))
    ,_remoteAddress(remoteAddress)
    ,_insertTimer(bind(&DBWorker::insertRevisionsNow, this))
    {
        registerHandler("getCheckpoint",    &DBWorker::handleGetCheckpoint);
        registerHandler("setCheckpoint",    &DBWorker::handleSetCheckpoint);
    }


    void DBWorker::_setCookie(alloc_slice setCookieHeader) {
        C4Error err;
        if (c4db_setCookie(_db, setCookieHeader, slice(_remoteAddress.hostname), &err)) {
            logVerbose("Set cookie: `%.*s`", SPLAT(setCookieHeader));
        } else {
            alloc_slice message = c4error_getMessage(err);
            warn("Unable to set cookie `%.*s`: %.*s (%d/%d)",
                 SPLAT(setCookieHeader), SPLAT(message), err.domain, err.code);
        }
    }


#pragma mark - CHECKPOINTS:


    // Implementation of public getCheckpoint(): Reads the local checkpoint & calls the callback
    void DBWorker::_getCheckpoint(CheckpointCallback callback) {
        alloc_slice body;
        bool dbIsEmpty = false;
        C4Error err;
        alloc_slice checkpointID(effectiveRemoteCheckpointDocID(&err));
        if (checkpointID) {
            c4::ref<C4RawDocument> doc( c4raw_get(_db,
                                                  kLocalCheckpointStore,
                                                  checkpointID,
                                                  &err) );
            if (doc)
                body = alloc_slice(doc->body);
            else if (isNotFoundError(err))
                err = {};
        }
        dbIsEmpty = c4db_getLastSequence(_db) == 0;
        callback(checkpointID, body, dbIsEmpty, err);
    }


    void DBWorker::_setCheckpoint(alloc_slice data, std::function<void()> onComplete) {
        C4Error err;
        alloc_slice checkpointID(effectiveRemoteCheckpointDocID(&err));
        if (checkpointID && c4raw_put(_db, kLocalCheckpointStore, checkpointID, nullslice, data, &err))
            log("Saved local checkpoint %.*s to db", SPLAT(checkpointID));
        else
            gotError(err);
        onComplete();
    }


    // Computes the ID of the checkpoint document.
    slice DBWorker::effectiveRemoteCheckpointDocID(C4Error *err) {
        if (_remoteCheckpointDocID.empty()) {
            // Simplistic default value derived from db UUID and remote URL:
            C4UUID privateUUID;
            if (!c4db_getUUIDs(_db, nullptr, &privateUUID, err))
                return nullslice;
            fleeceapi::Encoder enc;
            enc.beginArray();
            enc.writeString({&privateUUID, sizeof(privateUUID)});
            enc.writeString(_remoteAddress);
            enc.endArray();
            alloc_slice data = enc.finish();
            SHA1 digest(data);
            _remoteCheckpointDocID = string("cp-") + slice(&digest, sizeof(digest)).base64String();
        }
        return slice(_remoteCheckpointDocID);
    }


    bool DBWorker::getPeerCheckpointDoc(MessageIn* request, bool getting,
                                       slice &checkpointID, c4::ref<C4RawDocument> &doc) {
        checkpointID = request->property("client"_sl);
        if (!checkpointID) {
            request->respondWithError({"BLIP"_sl, 400, "missing checkpoint ID"_sl});
            return false;
        }
        log("Request to %s checkpoint '%.*s'",
            (getting ? "get" : "set"), SPLAT(checkpointID));

        C4Error err;
        doc = c4raw_get(_db, kPeerCheckpointStore, checkpointID, &err);
        if (!doc) {
            int status = isNotFoundError(err) ? 404 : 502;
            if (getting || (status != 404)) {
                request->respondWithError({"HTTP"_sl, status});
                return false;
            }
        }
        return true;
    }


    // Handles a "getCheckpoint" request by looking up a peer checkpoint.
    void DBWorker::handleGetCheckpoint(Retained<MessageIn> request) {
        c4::ref<C4RawDocument> doc;
        slice checkpointID;
        if (!getPeerCheckpointDoc(request, true, checkpointID, doc))
            return;
        MessageBuilder response(request);
        response["rev"_sl] = doc->meta;
        response << doc->body;
        request->respond(response);
    }


    // Handles a "setCheckpoint" request by storing a peer checkpoint.
    void DBWorker::handleSetCheckpoint(Retained<MessageIn> request) {
        C4Error err;
        c4::Transaction t(_db);
        if (!t.begin(&err))
            request->respondWithError(c4ToBLIPError(err));

        // Get the existing raw doc so we can check its revID:
        slice checkpointID;
        c4::ref<C4RawDocument> doc;
        if (!getPeerCheckpointDoc(request, false, checkpointID, doc))
            return;

        slice actualRev;
        unsigned long generation = 0;
        if (doc) {
            actualRev = (slice)doc->meta;
            char *end;
            generation = strtol((const char*)actualRev.buf, &end, 10);  //FIX: can fall off end
        }

        // Check for conflict:
        if (request->property("rev"_sl) != actualRev)
            return request->respondWithError({"HTTP"_sl, 409, "revision ID mismatch"_sl});

        // Generate new revID:
        char newRevBuf[30];
        slice rev = slice(newRevBuf, sprintf(newRevBuf, "%lu-cc", ++generation));

        // Save:
        if (!c4raw_put(_db, kPeerCheckpointStore, checkpointID, rev, request->body(), &err)
                || !t.commit(&err)) {
            return request->respondWithError(c4ToBLIPError(err));
        }

        // Success!
        MessageBuilder response(request);
        response["rev"_sl] = rev;
        request->respond(response);
    }


#pragma mark - CHANGES:


    static bool passesDocIDFilter(const DocIDSet &docIDs, slice docID) {
        return !docIDs || (docIDs->find(docID.asString()) != docIDs->end());
    }


    void DBWorker::getChanges(C4SequenceNumber since, DocIDSet docIDs, unsigned limit,
                              bool continuous, bool skipDeleted, bool getForeignAncestor,
                              Pusher *pusher)
    {
        enqueue(&DBWorker::_getChanges, since, docIDs, limit,
                continuous, skipDeleted, getForeignAncestor,
                Retained<Pusher>(pusher));
    }

    
    // A request from the Pusher to send it a batch of changes. Will respond by calling gotChanges.
    void DBWorker::_getChanges(C4SequenceNumber since, DocIDSet docIDs, unsigned limit,
                               bool continuous, bool skipDeleted, bool getForeignAncestors,
                               Retained<Pusher> pusher)
    {
        if (!connection())
            return;
        log("Reading up to %u local changes since #%llu", limit, since);
        if (_firstChangeSequence == 0)
            _firstChangeSequence = since + 1;

        // Run a by-sequence enumerator to find the changed docs:
        vector<Rev> changes;
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (!getForeignAncestors)
            options.flags &= ~kC4IncludeBodies;
        if (!skipDeleted)
            options.flags |= kC4IncludeDeleted;
        c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(_db, since, &options, &error);
        if (e) {
            changes.reserve(limit);
            while (c4enum_next(e, &error) && limit > 0) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                if (passesDocIDFilter(docIDs, info.docID)) {
                    alloc_slice foreignAncestor;
                    if (getForeignAncestors) {
                        // For proposeChanges, find the nearest foreign ancestor of the current rev:
                        if (!getForeignAncestor(e, foreignAncestor, &error)) {
                            if (error.code)
                                gotDocumentError(info.docID, error, true, false);
                            continue; // skip to next sequence
                        }
                    }
                    changes.emplace_back(info, foreignAncestor);
                    --limit;
                }
            }
        }

        markRevsSynced(changes, nullptr);

        if (continuous && limit > 0 && !_changeObserver) {
            // Reached the end of history; now start observing for future changes
            _pusher = pusher;
            _pushDocIDs = docIDs;
            _changeObserver = c4dbobs_create(_db,
                                             [](C4DatabaseObserver* observer, void *context) {
                                                 auto self = (DBWorker*)context;
                                                 self->enqueue(&DBWorker::dbChanged);
                                             },
                                             this);
            logDebug("Started DB observer");
        }

        pusher->gotChanges(changes, error);
    }


    // Callback from the C4DatabaseObserver when the database has changed
    void DBWorker::dbChanged() {
        static const uint32_t kMaxChanges = 100;
        C4DatabaseChange c4changes[kMaxChanges];
        bool external;
        uint32_t nChanges;
        vector<Rev> changes;
        while (true) {
            nChanges = c4dbobs_getChanges(_changeObserver, c4changes, kMaxChanges, &external);
            if (nChanges == 0)
                break;
            log("Notified of %u db changes #%llu ... #%llu",
                nChanges, c4changes[0].sequence, c4changes[nChanges-1].sequence);
            changes.clear();
            C4DatabaseChange *c4change = c4changes;
            for (uint32_t i = 0; i < nChanges; ++i, ++c4change) {
                if (passesDocIDFilter(_pushDocIDs, c4change->docID)) {
                    changes.emplace_back(c4change->docID, c4change->revID,
                                         c4change->sequence, c4change->bodySize);
                }
                // Note: we send tombstones even if the original getChanges() call specified
                // skipDeletions. This is intentional; skipDeletions applies only to the initial
                // dump of existing docs, not to 'live' changes.
            }

            if (!changes.empty()) {
                markRevsSynced(changes, nullptr);
                _pusher->gotChanges(changes, {});
            }
        }
    }


    void DBWorker::_connectionClosed() {
        Worker::_connectionClosed();
        _pusher = nullptr;                      // breaks ref-cycle
        _changeObserver = nullptr;
    }


    // For proposeChanges, find the latest ancestor of the current rev that is known to the server.
    // This is a rev that's either marked as foreign (came from the server), or whose sequence is
    // prior to the checkpoint (has already been pushed to the server.)
    bool DBWorker::getForeignAncestor(C4DocEnumerator *e, alloc_slice &foreignAncestor, C4Error *outError) {
        c4::ref<C4Document> doc = c4enum_getDocument(e, outError);
        if (!doc)
            return false;
        if (doc->selectedRev.flags & kRevIsForeign) {
            outError->code = 0;
            return false;       // skip this, it's not a locally created rev
        }
        while (c4doc_selectParentRevision(doc)) {
            if ((doc->selectedRev.flags & kRevIsForeign)
                        || doc->selectedRev.sequence < _firstChangeSequence) {
                foreignAncestor = slice(doc->selectedRev.revID);
                return true;
            }
        }
        foreignAncestor = nullslice;
        return true;
    }


    // Mark all of these revs as synced, which flags them as kRevKeepBody if they're still current.
    // This is actually premature because those revs haven't been pushed yet; but if we
    // wait until after the push, the doc may have been updated again and the body of the
    // pushed revision lost. By doing it early we err on the side of correctness and may
    // save some revision bodies unnecessarily, which isn't that bad.
    bool DBWorker::markRevsSynced(const vector<Rev> changes, C4Error *outError) {
        if (changes.empty())
            return true;
        c4::Transaction t(_db);
        if (!t.begin(outError))
            return false;
        for (auto i = changes.begin(); i != changes.end(); ++i)
            c4db_markSynced(_db, i->docID, i->sequence);
        return t.commit(outError);
    }


    // Called by the Puller; handles a "changes" or "proposeChanges" message by checking which of
    // the changes don't exist locally, and returning a bit-vector indicating them.
    void DBWorker::_findOrRequestRevs(Retained<MessageIn> req,
                                     function<void(vector<bool>)> callback) {
        // Iterate over the array in the message, seeing whether I have each revision:
        bool proposed = (req->property("Profile"_sl) == "proposeChanges"_sl);
        auto changes = req->JSONBody().asArray();
        if (willLog() && !changes.empty()) {
            if (proposed) {
                log("Looking up %u proposed revisions in the db", changes.count());
            } else {
                alloc_slice firstSeq(changes[0].asArray()[0].toString());
                alloc_slice lastSeq (changes[changes.count()-1].asArray()[0].toString());
                log("Looking up %u revisions in the db (seq '%.*s'..'%.*s')",
                    changes.count(), SPLAT(firstSeq), SPLAT(lastSeq));
            }
        }

        MessageBuilder response(req);
        response["maxHistory"_sl] = c4db_getMaxRevTreeDepth(_db);
        response["blobs"_sl] = "true"_sl;
        vector<bool> whichRequested(changes.count());
        unsigned itemsWritten = 0, requested = 0;
        vector<alloc_slice> ancestors;
        auto &encoder = response.jsonBody();
        encoder.beginArray();
        int i = -1;
        for (auto item : changes) {
            ++i;
            // Look up each revision in the `req` list:
            auto change = item.asArray();
            slice docID = change[proposed ? 0 : 1].asString();
            slice revID = change[proposed ? 1 : 2].asString();
            if (docID.size == 0 || revID.size == 0) {
                warn("Invalid entry in 'changes' message");
                continue;     // ???  Should this abort the replication?
            }

            if (proposed) {
                // "proposeChanges" entry: [docID, revID, parentRevID?, bodySize?]
                slice parentRevID = change[2].asString();
                if (parentRevID.size == 0)
                    parentRevID = nullslice;
                int status = findProposedChange(docID, revID, parentRevID);
                if (status == 0) {
                    ++requested;
                    whichRequested[i] = true;
                } else {
                    log("Rejecting proposed change '%.*s' #%.*s (status %d)",
                        SPLAT(docID), SPLAT(revID), status);
                    while (itemsWritten++ < i)
                        encoder.writeInt(0);
                    encoder.writeInt(status);
                }

            } else {
                // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
                if (!findAncestors(docID, revID, ancestors)) {
                    // I don't have this revision, so request it:
                    ++requested;
                    whichRequested[i] = true;

                    while (itemsWritten++ < i)
                        encoder.writeInt(0);
                    encoder.beginArray();
                    for (slice ancestor : ancestors)
                        encoder.writeString(ancestor);
                    encoder.endArray();
                }
            }
        }
        encoder.endArray();

        if (callback)
            callback(whichRequested);

        log("Responding w/request for %u revs", requested);
        req->respond(response);
    }


    // Returns true if revision exists; else returns false and sets ancestors to an array of
    // ancestor revisions I do have (empty if doc doesn't exist at all)
    bool DBWorker::findAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &err);
        if (doc && c4doc_selectRevision(doc, revID, false, &err)) {
            // I already have this revision. Make sure it's marked as foreign:
            if (!(doc->selectedRev.flags & kRevIsForeign)) {
                //TODO: Mark rev as foreign in DB
            }
            return true;
        }

        ancestors.resize(0);
        if (doc) {
            // Revision isn't found, but look for ancestors:
            if (c4doc_selectFirstPossibleAncestorOf(doc, revID)) {
                do {
                    ancestors.emplace_back(doc->selectedRev.revID);
                } while (c4doc_selectNextPossibleAncestorOf(doc, revID)
                         && ancestors.size() < kMaxPossibleAncestors);
            }
        } else if (!isNotFoundError(err)) {
            gotError(err);
        }
        return false;
    }


    // Checks whether the revID (if any) is really current for the given doc.
    // Returns an HTTP-ish status code: 0=OK, 409=conflict, 500=internal error
    int DBWorker::findProposedChange(slice docID, slice revID, slice parentRevID) {
        C4Error err;
        //OPT: We don't need the document body, just its metadata, but there's no way to say that
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &err);
        if (!doc) {
            if (isNotFoundError(err)) {
                // Doc doesn't exist; it's a conflict if the peer thinks it does:
                return parentRevID ? 409 : 0;
            } else {
                gotError(err);
                return 500;
            }
        } else if (slice(doc->revID) == revID) {
            // I already have this revision:
            return 304;
        } else if (!parentRevID) {
            // Peer is creating new doc; that's OK if doc is currently deleted:
            return (doc->flags & kDocDeleted) ? 0 : 409;
        } else if (slice(doc->revID) != parentRevID) {
            // Peer's revID isn't current, so this is a conflict:
            return 409;
        } else {
            // I don't have this revision and it's not a conflict, so I want it!
            return 0;
        }
    }


#pragma mark - SENDING REVISIONS:


    // Sends a document revision in a "rev" request.
    void DBWorker::_sendRevision(RevRequest request,
                                MessageProgressCallback onProgress)
    {
        if (!connection())
            return;
        logVerbose("Sending revision '%.*s' #%.*s",
                   SPLAT(request.docID), SPLAT(request.revID));
        C4Error c4err;
        slice revisionBody;
        C4RevisionFlags revisionFlags = 0;
        string history;
        Dict root;
        int blipError = 0;
        c4::ref<C4Document> doc = c4doc_get(_db, request.docID, true, &c4err);
        if (doc && c4doc_selectRevision(doc, request.revID, true, &c4err)) {
            revisionBody = slice(doc->selectedRev.body);
            if (revisionBody) {
                root = Value::fromTrustedData(revisionBody).asDict();
                if (!root) {
                    blipError = 500;
                    c4err = c4error_make(LiteCoreDomain, kC4ErrorCorruptData,
                                         "Unparseable revision body"_sl);
                } else if (root.count() == 0) {
                    root = Dict(); // no sense encoding an empty body later
                }
            }
            revisionFlags = doc->selectedRev.flags;

            // Generate the revision history string:
            set<pure_slice> ancestors(request.ancestorRevIDs.begin(), request.ancestorRevIDs.end());
            stringstream historyStream;
            for (int n = 0; n < request.maxHistory; ++n) {
                if (!c4doc_selectParentRevision(doc))
                    break;
                slice revID = doc->selectedRev.revID;
                if (n > 0)
                    historyStream << ',';
                historyStream << fleeceapi::asstring(revID);
                if (ancestors.find(revID) != ancestors.end())
                    break;
            }
            history = historyStream.str();
        } else {
            // Well, this is a pickle. I'm supposed to send a revision that I can't read.
            // I need to send a "rev" message, and I can't use a BLIP error response (because this
            // is a request not a response) so I'll add an "error" property to it instead of body.
            warn("sendRevision: Couldn't get '%.*s'/%.*s from db: %d/%d",
                 SPLAT(request.docID), SPLAT(request.revID), c4err.domain, c4err.code);
            doc = nullptr;
            if (c4err.domain == LiteCoreDomain && c4err.code == kC4ErrorNotFound)
                blipError = 404;
            else if (c4err.domain == LiteCoreDomain && c4err.code == kC4ErrorDeleted)
                blipError = 410;
            else
                blipError = 500;
        }

        // Now send the BLIP message:
        MessageBuilder msg("rev"_sl);
        msg.noreply = !onProgress;
        msg.compressed = (revisionBody.size >= kMinBodySizeToCompress);
        msg["id"_sl] = request.docID;
        msg["rev"_sl] = request.revID;
        msg["sequence"_sl] = request.sequence;
        if (revisionFlags & kRevDeleted)
            msg["deleted"_sl] = "1"_sl;
        if (!history.empty())
            msg["history"_sl] = history;
        if (blipError)
            msg["error"_sl] = blipError;

        // Write doc body as JSON:
        if (root) {
            auto &bodyEncoder = msg.jsonBody();
            auto sk = c4db_getFLSharedKeys(_db);
            bodyEncoder.setSharedKeys(sk);
            if (request.legacyAttachments && (revisionFlags & kRevHasAttachments))
                writeRevWithLegacyAttachments(bodyEncoder, root, sk);
            else
                bodyEncoder.writeValue(root);
        } else {
            msg.write("{}"_sl);
        }
        sendRequest(msg, onProgress);
    }


    void DBWorker::writeRevWithLegacyAttachments(Encoder& enc, Dict root, FLSharedKeys sk) {
        enc.beginDict();

        // Write existing properties except for _attachments:
        Dict oldAttachments;
        for (Dict::iterator i(root, sk); i; ++i) {
            slice key = i.keyString();
            if (key == slice(kC4LegacyAttachmentsProperty)) {
                oldAttachments = i.value().asDict();    // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey("_attachments"_sl);
        enc.beginDict();
        // First pre-existing legacy attachments, if any:
        for (Dict::iterator i(oldAttachments, sk); i; ++i) {
            slice key = i.keyString();
            if (!key.hasPrefix("blob_"_sl)) {
                // TODO: Should skip this entry if a blob with the same digest exists
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Then entries for blobs found in the document:
        unsigned n = 0;
        IncomingRev::findBlobReferences(root, sk, [&](Dict dict, C4BlobKey blobKey) {
            char attName[32];
            sprintf(attName, "blob_%u", ++n);   // TODO: Mnemonic name based on JSON key path
            enc.writeKey(slice(attName));
            enc.beginDict();
            for (Dict::iterator i(dict, sk); i; ++i) {
                slice key = i.keyString();
                if (key != slice(kC4ObjectTypeProperty) && key != "stub"_sl) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.writeKey("stub"_sl);
            enc.writeBool(true);
            enc.writeKey("revpos"_sl);
            enc.writeInt(1);
            enc.endDict();
        });
        enc.endDict();

        enc.endDict();
    }


#pragma mark - INSERTING REVISIONS:


    void DBWorker::insertRevision(RevToInsert *rev) {
        lock_guard<mutex> lock(_revsToInsertMutex);
        if (!_revsToInsert) {
            _revsToInsert.reset(new vector<RevToInsert*>);
            _revsToInsert->reserve(500);
            enqueueAfter(kInsertionDelay, &DBWorker::_insertRevisionsNow);
        }
        _revsToInsert->push_back(rev);
    }


    void DBWorker::_insertRevisionsNow() {
        __typeof(_revsToInsert) revs;
        {
            lock_guard<mutex> lock(_revsToInsertMutex);
            revs = move(_revsToInsert);
            _revsToInsert.reset();
        }

        logVerbose("Inserting %zu revs:", revs->size());
        Stopwatch st;

        C4Error transactionErr;
        c4::Transaction transaction(_db);
        if (transaction.begin(&transactionErr)) {
            SharedEncoder enc(c4db_getSharedFleeceEncoder(_db));
            
            for (auto &rev : *revs) {
                // Add a revision:
                logVerbose("    {'%.*s' #%.*s}", SPLAT(rev->docID), SPLAT(rev->revID));
                vector<C4String> history;
                history.reserve(10);
                history.push_back(rev->revID);
                for (const void *pos=rev->historyBuf.buf, *end = rev->historyBuf.end(); pos < end;) {
                    auto comma = slice(pos, end).findByteOrEnd(',');
                    history.push_back(slice(pos, comma));
                    pos = comma + 1;
                }

                // rev->body is Fleece, but sadly we can't insert it directly because it doesn't
                // use the db's SharedKeys, so all of its Dict keys are strings. Putting this into
                // the db would cause failures looking up those keys (see #156). So re-encode:
                Value root = Value::fromTrustedData(rev->body);
                enc.writeValue(root);
                alloc_slice bodyForDB = enc.finish();
                enc.reset();
                rev->body = nullslice;

                C4DocPutRequest put = {};
                put.body = bodyForDB;
                put.docID = rev->docID;
                put.revFlags = rev->flags | kRevIsForeign | kRevKeepBody;
                put.existingRevision = true;
                put.allowConflict = true;
                put.history = history.data();
                put.historyCount = history.size();
                put.save = true;

                C4Error docErr;
                c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &docErr);
                if (!doc) {
                    warn("Failed to insert '%.*s' #%.*s : error %d/%d",
                         SPLAT(rev->docID), SPLAT(rev->revID), docErr.domain, docErr.code);
                    if (rev->onInserted)
                        rev->onInserted(docErr);
                    rev = nullptr;
                } else if (hasConflict(doc)) {
                    // Notify that rev was inserted but caused a conflict:
                    log("Created conflict with '%.*s' #%.*s",
                        SPLAT(rev->docID), SPLAT(rev->revID));
                    gotDocumentError(rev->docID, {LiteCoreDomain, kC4ErrorConflict}, false, true);
                }
            }
        }

        if (transaction.active() && transaction.commit(&transactionErr))
            transactionErr = { };
        else
            warn("Transaction failed!");

        // Notify all revs (that didn't already fail):
        for (auto rev : *revs) {
            if (rev && rev->onInserted)
                rev->onInserted(transactionErr);
        }

        if (transactionErr.code)
            gotError(transactionErr);
        else {
            double t = st.elapsed();
            log("Inserted %zu revs in %.2fms (%.0f/sec)", revs->size(), t*1000, revs->size()/t);
        }
    }

} }
