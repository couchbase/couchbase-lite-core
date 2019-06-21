//
// Replicator+Checkpoints.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "Pusher.hh"
#include "Address.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "c4.hh"
#include "BLIP.hh"
#include "RevID.hh"
#include <chrono>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {


    // Returns a string that uniquely identifies the remote database; by default its URL,
    // or the 'remoteUniqueID' option if that's present (for P2P dbs without stable URLs.)
    string Replicator::remoteDBIDString() const {
        slice uniqueID = _options.properties[kC4ReplicatorOptionRemoteDBUniqueID].asString();
        if (uniqueID)
            return string(uniqueID);
        return string(_remoteURL);
    }


    void Replicator::setCookie(slice setCookieHeader) {
        Address addr(_remoteURL);
        C4Error err;
        bool ok = _db->use<bool>([&](C4Database *db) {
            return c4db_setCookie(db, setCookieHeader, addr.hostname, addr.path, &err);
        });
        if (ok) {
            logVerbose("Set cookie: `%.*s`", SPLAT(setCookieHeader));
        } else {
            alloc_slice message = c4error_getDescription(err);
            warn("Unable to set cookie `%.*s`: %.*s",
                 SPLAT(setCookieHeader), SPLAT(message));
        }
    }


#pragma mark - CHECKPOINTS:


    alloc_slice Replicator::_checkpointFromID(const slice &checkpointID, C4Error* err) {
        alloc_slice body;
        if (checkpointID) {
            const c4::ref<C4RawDocument> doc( _db->getRawDoc(constants::kLocalCheckpointStore,
                                                        checkpointID,
                                                        err) );
            if (doc)
                body = alloc_slice(doc->body);
        }
        return body;
    }


    // Reads the local checkpoint
    Replicator::CheckpointResult Replicator::getCheckpoint() {
        C4Error err;
        alloc_slice checkpointID = alloc_slice(effectiveRemoteCheckpointDocID(&err));
        alloc_slice body = _checkpointFromID(checkpointID, &err);
        if(body.size == 0 && isNotFoundError(err)) {
            string oldCheckpointValue = _getOldCheckpoint(&err);
            if(oldCheckpointValue.empty()) {
                if(isNotFoundError(err)) {
                    err = {};
                }
            } else {
                checkpointID = alloc_slice(oldCheckpointValue);
                body = alloc_slice(_checkpointFromID(checkpointID, &err));
                if(body.size == 0) {
                    if(isNotFoundError(err)) {
                        err = {};
                    }
                }
            }
        }

        const bool dbIsEmpty = _db->use<bool>([&](C4Database *db) {
            return c4db_getLastSequence(db) == 0;
        });
        return {checkpointID, body, dbIsEmpty, err};
    }


    // Gets a checkpoint based on the DB's prior UUID before it was copied; called by getCheckpoint
    string Replicator::_getOldCheckpoint(C4Error* err) {
        const c4::ref<C4RawDocument> doc = _db->getRawDoc(kC4InfoStore,
                                                          constants::kPreviousPrivateUUIDKey,
                                                          err);
        if(!doc) {
            err->domain = LiteCoreDomain;
            err->code = kC4ErrorNotFound;
            return string();
        }

        C4UUID oldUUID = *(C4UUID*)doc->body.buf;
        return effectiveRemoteCheckpointDocID(&oldUUID, err);
    }


    // Saves a local checkpoint
    void Replicator::setCheckpoint(slice data) {
        C4Error err;
        const auto checkpointID = effectiveRemoteCheckpointDocID(&err);
        if (!checkpointID)
            gotError(err);
        else {
            bool ok = _db->use<bool>([&](C4Database *db) {
                _db->markRevsSyncedNow();
                return c4raw_put(db, constants::kLocalCheckpointStore,
                                 checkpointID, nullslice, data, &err);
            });
            if (ok)
                logInfo("Saved local checkpoint %.*s to db", SPLAT(checkpointID));
            else
                gotError(err);
        }
    }


    slice Replicator::effectiveRemoteCheckpointDocID(C4Error* err) {
        if(_remoteCheckpointDocID.empty()) {
            C4UUID privateID;
            bool ok =  _db->use<bool>([&](C4Database *db) {
                return c4db_getUUIDs(db, nullptr, &privateID, err);
            });
            if (!ok) {
                return nullslice;
            }

            _remoteCheckpointDocID = effectiveRemoteCheckpointDocID(&privateID, err);
        }

        return slice(_remoteCheckpointDocID);
    }


    // Writes a Value to an Encoder, substituting null if the value is an empty array.
    static void writeValueOrNull(fleece::Encoder &enc, Value val) {
        auto a = val.asArray();
        if (!val || (a && a.empty()))
            enc.writeNull();
        else
            enc.writeValue(val);
    }


    // Computes the ID of the checkpoint document.
    string Replicator::effectiveRemoteCheckpointDocID(const C4UUID *localUUID, C4Error *err) {
        // Derive docID from from db UUID, remote URL, channels, filter, and docIDs.
        Array channels = _options.channels();
            Value filter = _options.properties[kC4ReplicatorOptionFilter];
        const Value filterParams = _options.properties[kC4ReplicatorOptionFilterParams];
        Array docIDs = _options.docIDs();

        // Compute the ID by writing the values to a Fleece array, then taking a SHA1 digest:
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString({localUUID, sizeof(C4UUID)});

        enc.writeString(remoteDBIDString());
        if (!channels.empty() || !docIDs.empty() || filter) {
            // Optional stuff:
            writeValueOrNull(enc, channels);
            writeValueOrNull(enc, filter);
            writeValueOrNull(enc, filterParams);
            writeValueOrNull(enc, docIDs);
        }
        enc.endArray();
        const alloc_slice data = enc.finish();
        SHA1 digest(data);
        string finalProduct = string("cp-") + slice(&digest, sizeof(digest)).base64String();
        logVerbose("Checkpoint doc ID = %s", finalProduct.c_str());
        return finalProduct;
    }


    // Reads the doc in which a peer's remote checkpoint is saved.
    bool Replicator::getPeerCheckpointDoc(MessageIn* request, bool getting,
                                          slice &checkpointID, c4::ref<C4RawDocument> &doc) const
    {
        checkpointID = request->property("client"_sl);
        if (!checkpointID) {
            request->respondWithError({"BLIP"_sl, 400, "missing checkpoint ID"_sl});
            return false;
        }
        logInfo("Request to %s checkpoint '%.*s'",
            (getting ? "get" : "set"), SPLAT(checkpointID));

        C4Error err;
        doc = _db->getRawDoc(constants::kPeerCheckpointStore, checkpointID, &err);
        if (!doc) {
            const int status = isNotFoundError(err) ? 404 : 502;
            if (getting || (status != 404)) {
                request->respondWithError({"HTTP"_sl, status});
                return false;
            }
        }
        return true;
    }


    // Handles a "getCheckpoint" request by looking up a peer checkpoint.
    void Replicator::handleGetCheckpoint(Retained<MessageIn> request) {
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
    void Replicator::handleSetCheckpoint(Retained<MessageIn> request) {
        char newRevBuf[30];
        alloc_slice rev;
        bool needsResponse = false;
        _db->use([&](C4Database *db) {
            C4Error err;
            c4::Transaction t(db);
            if (!t.begin(&err)) {
                request->respondWithError(c4ToBLIPError(err));
                return;
            }

            // Get the existing raw doc so we can check its revID:
            slice checkpointID;
            c4::ref<C4RawDocument> doc;
            if (!getPeerCheckpointDoc(request, false, checkpointID, doc))
                return;

            slice actualRev;
            unsigned long generation = 0;
            if (doc) {
                actualRev = (slice)doc->meta;
                try {
                    revid parsedRev(actualRev);
                    generation = parsedRev.generation();
                } catch(error &e) {
                    if(e.domain == error::Domain::LiteCore
                            && e.code == error::LiteCoreError::CorruptRevisionData) {
                        actualRev = nullslice;
                    } else {
                        throw;
                    }
                }
            }

            // Check for conflict:
            if (request->property("rev"_sl) != actualRev) {
                request->respondWithError({"HTTP"_sl, 409, "revision ID mismatch"_sl});
                return;
            }

            // Generate new revID:
            rev = slice(newRevBuf, sprintf(newRevBuf, "%lu-cc", ++generation));

            // Save:
            if (!c4raw_put(db, constants::kPeerCheckpointStore,
                           checkpointID, rev, request->body(), &err)
                    || !t.commit(&err)) {
                request->respondWithError(c4ToBLIPError(err));
                return;
            }

            needsResponse = true;
        });

        // In other words, an error response was generated above if this
        // is false
        if(!needsResponse) {
            return;
        }

        // Success!
        MessageBuilder response(request);
        response["rev"_sl] = rev;
        request->respond(response);
    }

} }
