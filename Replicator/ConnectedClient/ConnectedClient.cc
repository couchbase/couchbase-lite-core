//
// ConnectedClient.cc
//
// Copyright © 2022 Couchbase. All rights reserved.
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

#include "ConnectedClient.hh"
#include "c4BlobStoreTypes.h"
#include "c4Document.hh"
#include "c4SocketTypes.h"
#include "Headers.hh"
#include "MessageBuilder.hh"
#include "NumConversion.hh"
#include "WebSocketInterface.hh"
#include "c4Internal.hh"

namespace litecore::client {
    using namespace std;
    using namespace fleece;
    using namespace litecore::actor;
    using namespace blip;


    ConnectedClient::ConnectedClient(websocket::WebSocket* webSocket,
                                     Delegate& delegate,
                                     fleece::AllocedDict options)
    :Worker(new Connection(webSocket, options, *this),
            nullptr, nullptr, nullptr, "Client")
    ,_delegate(&delegate)
    ,_status(kC4Stopped)
    {
        _importance = 2;
    }


    void ConnectedClient::setStatus(ActivityLevel status) {
        if (status != _status) {
            _status = status;
            
            LOCK(_mutex);
            if (_delegate)
                _delegate->clientStatusChanged(this, status);
        }
    }


    void ConnectedClient::start() {
        asCurrentActor([=] {
            logInfo("Connecting...");
            Assert(_status == kC4Stopped);
            setStatus(kC4Connecting);
            connection().start();
            _selfRetain = this;     // retain myself while the connection is open
        });
    }

    void ConnectedClient::stop() {
        asCurrentActor([=] {
            _disconnect(websocket::kCodeNormal, {});
        });
    }


    void ConnectedClient::_disconnect(websocket::CloseCode closeCode, slice message) {
        if (connected()) {
            logInfo("Disconnecting...");
            connection().close(closeCode, message);
            setStatus(kC4Stopping);
        }
    }

    void ConnectedClient::terminate() {
        LOCK(_mutex);
        _delegate = nullptr;
    }


#pragma mark - BLIP DELEGATE:


    void ConnectedClient::onTLSCertificate(slice certData) {
        LOCK(_mutex);
        if (_delegate)
            _delegate->clientGotTLSCertificate(this, certData);
    }


    void ConnectedClient::onHTTPResponse(int status, const websocket::Headers &headers) {
        asCurrentActor([=] {
            logVerbose("Got HTTP response from server, status %d", status);
            {
                LOCK(_mutex);
                if (_delegate)
                    _delegate->clientGotHTTPResponse(this, status, headers);
            }
            
            if (status == 101 && !headers["Sec-WebSocket-Protocol"_sl]) {
                gotError(C4Error::make(WebSocketDomain, kWebSocketCloseProtocolError,
                                       "Incompatible replication protocol "
                                       "(missing 'Sec-WebSocket-Protocol' response header)"_sl));
            }
        });
    }


    void ConnectedClient::onConnect() {
        asCurrentActor([=] {
        logInfo("Connected!");
        if (_status != kC4Stopping)       // skip this if stop() already called
            setStatus(kC4Idle);
        });
    }


    void ConnectedClient::onClose(Connection::CloseStatus status, Connection::State state) {
        asCurrentActor([=]() mutable {
            logInfo("Connection closed with %-s %d: \"%.*s\" (state=%d)",
                    status.reasonName(), status.code, FMTSLICE(status.message), state);

            bool closedByPeer = (_status != kC4Stopping);
            setStatus(kC4Stopped);

            _connectionClosed();

            if (status.isNormal() && closedByPeer) {
                logInfo("I didn't initiate the close; treating this as code 1001 (GoingAway)");
                status.code = websocket::kCodeGoingAway;
                status.message = alloc_slice("WebSocket connection closed by peer");
            }

            static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain,
                NetworkDomain, LiteCoreDomain};

            // If this was an unclean close, set my error property:
            if (status.reason != websocket::kWebSocketClose || status.code != websocket::kCodeNormal) {
                int code = status.code;
                C4ErrorDomain domain;
                if (status.reason < sizeof(kDomainForReason)/sizeof(C4ErrorDomain)) {
                    domain = kDomainForReason[status.reason];
                } else {
                    domain = LiteCoreDomain;
                    code = kC4ErrorRemoteError;
                }
                gotError(C4Error::make(domain, code, status.message));
            }
            {
                LOCK(_mutex);
                if (_delegate)
                    _delegate->clientConnectionClosed(this, status);
            }

            _selfRetain = nullptr;  // balances the self-retain in start()
        });
    }


    // This only gets called if none of the registered handlers were triggered.
    void ConnectedClient::onRequestReceived(MessageIn *msg) {
        warn("Received unrecognized BLIP request #%" PRIu64 " with Profile '%.*s', %zu bytes",
             msg->number(), FMTSLICE(msg->profile()), msg->body().size);
        msg->notHandled();
    }


#pragma mark - REQUESTS:


    // Returns the error status of a response (including a NULL response, i.e. disconnection)
    C4Error ConnectedClient::responseError(MessageIn *response) {
        C4Error error;
        if (!response) {
            // Disconnected!
            error = status().error;
            if (!error)
                error = C4Error::make(LiteCoreDomain, kC4ErrorIOError, "network connection lost");
            // TODO: Use a better default error than the one above
        } else if (response->isError()) {
            error = blipToC4Error(response->getError());
        } else {
            error = {};
        }
        if (error)
            logError("Connected Client got error response %s", error.description().c_str());
        return error;
    }


    Async<DocResponse> ConnectedClient::getDoc(slice docID_,
                                                      slice collectionID_,
                                                      slice unlessRevID_,
                                                      bool asFleece)
    {
        // Not yet running on Actor thread...
        logInfo("getDoc(\"%.*s\")", FMTSLICE(docID_));
        alloc_slice docID(docID_);
        MessageBuilder req("getRev");
        req["id"] = docID;
        req["ifNotRev"] = unlessRevID_;

        return sendAsyncRequest(req)
            .then([=](Retained<blip::MessageIn> response) -> Async<DocResponse> {
                logInfo("...getDoc got response");

                if (C4Error err = responseError(response))
                    return err;

                DocResponse docResponse {
                    docID,
                    alloc_slice(response->property("rev")),
                    response->body(),
                    response->boolProperty("deleted")
                };

                if (asFleece && docResponse.body) {
                    FLError flErr;
                    docResponse.body = FLData_ConvertJSON(docResponse.body, &flErr);
                    if (!docResponse.body)
                        C4Error::raise(FleeceDomain, flErr, "Unparseable JSON response from server");
                }
                return docResponse;
            });
    }


    Async<alloc_slice> ConnectedClient::getBlob(C4BlobKey blobKey,
                                                bool compress)
    {
        // Not yet running on Actor thread...
        auto digest = blobKey.digestString();
        logInfo("getAttachment(<%s>)", digest.c_str());
        MessageBuilder req("getAttachment");
        req["digest"] = digest;
        if (compress)
            req["compress"] = "true";

        return sendAsyncRequest(req)
            .then([=](Retained<blip::MessageIn> response) -> Async<alloc_slice> {
                logInfo("...getAttachment got response");
                if (C4Error err = responseError(response))
                    return err;
                return response->body();
            });
    }


    Async<void> ConnectedClient::putDoc(slice docID_,
                                           slice collectionID_,
                                           slice revID_,
                                           slice parentRevID_,
                                           C4RevisionFlags revisionFlags,
                                           slice fleeceData_)
    {
        // Not yet running on Actor thread...
        logInfo("putDoc(\"%.*s\", \"%.*s\")", FMTSLICE(docID_), FMTSLICE(revID_));
        MessageBuilder req("putRev");
        req.compressed = true;
        req["id"] = docID_;
        req["rev"] = revID_;
        req["history"] = parentRevID_;
        if (revisionFlags & kRevDeleted)
            req["deleted"] = "1";

        if (fleeceData_.size > 0) {
            // TODO: Encryption!!
            // TODO: Convert blobs to legacy attachments
            req.jsonBody().writeValue(FLValue_FromData(fleeceData_, kFLTrusted));
        } else {
            req.write("{}");
        }

        return sendAsyncRequest(req)
            .then([=](Retained<blip::MessageIn> response) -> Async<void> {
                logInfo("...putDoc got response");
                return Async<void>(responseError(response));
            });
    }


    Async<void> ConnectedClient::observeCollection(slice collectionID_,
                                                      CollectionObserver callback_)
    {
        return asCurrentActor([this,
                               collectionID = alloc_slice(collectionID_),
                               observe = !!callback_,
                               callback = move(callback_)] () -> Async<void> {
            logInfo("observeCollection(%.*s)", FMTSLICE(collectionID));

            bool sameSubState = (observe == !!_observer);
            _observer = move(callback);
            if (sameSubState)
                return C4Error{};

            MessageBuilder req;
            if (observe) {
                if (!_registeredChangesHandler) {
                    registerHandler("changes", &ConnectedClient::handleChanges);
                    _registeredChangesHandler = true;
                }
                req.setProfile("subChanges");
                req["since"]      = "NOW";
                req["continuous"] = true;
            } else {
                req.setProfile("unsubChanges");
            }

            return sendAsyncRequest(req)
                .then([=](Retained<blip::MessageIn> response) {
                    logInfo("...observeCollection got response");
                    return Async<void>(responseError(response));
                });
        });
    }


    void ConnectedClient::handleChanges(Retained<blip::MessageIn> req) {
        // The code below is adapted from RevFinder::handleChangesNow and RevFinder::findRevs.
        auto inChanges = req->JSONBody().asArray();
        if (!inChanges && req->body() != "null"_sl) {
            warn("Invalid body of 'changes' message");
            req->respondWithError(400, "Invalid JSON body"_sl);
            return;
        }

        // "changes" expects a response with an array of which items we want "rev" messages for.
        // We don't actually want any. An empty array will indicate that.
        if (!req->noReply()) {
            MessageBuilder response(req);
            auto &enc = response.jsonBody();
            enc.beginArray();
            enc.endArray();
            req->respond(response);
        }

        if (_observer && !inChanges.empty()) {
            logInfo("Received %u doc changes from server", inChanges.count());
            // Convert the JSON change list into a vector:
            vector<C4CollectionObserver::Change> outChanges;
            outChanges.reserve(inChanges.count());
            for (auto item : inChanges) {
                // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
                auto inChange = item.asArray();
                slice docID = inChange[1].asString();
                slice revID = inChange[2].asString();
                if (validateDocAndRevID(docID, revID)) {
                    auto &outChange = outChanges.emplace_back();
                    outChange.sequence = C4SequenceNumber{inChange[0].asUnsigned()};
                    outChange.docID    = docID;
                    outChange.revID    = revID;
                    outChange.flags    = 0;
                    int64_t deletion   = inChange[3].asInt();
                    outChange.bodySize = fleece::narrow_cast<uint32_t>(inChange[4].asUnsigned());

                    // In SG 2.x "deletion" is a boolean flag, 0=normal, 1=deleted.
                    // SG 3.x adds 2=revoked, 3=revoked+deleted, 4=removal (from channel)
                    if (deletion & 0b001)
                        outChange.flags |= kRevDeleted;
                    if (deletion & 0b110)
                        outChange.flags |= kRevPurged;
                }
            }

            // Finally call the observer callback:
            try {
                _observer(outChanges);
            } catch (...) {
                logError("ConnectedClient observer threw exception: %s",
                         C4Error::fromCurrentException().description().c_str());
            }
        }
    }


    bool ConnectedClient::validateDocAndRevID(slice docID, slice revID) {
        bool valid;
        if (!C4Document::isValidDocID(docID))
            valid = false;
        else if (_remoteUsesVersionVectors)
            valid = revID.findByte('@') && !revID.findByte('*');     // require absolute form
        else
            valid = revID.findByte('-');
        if (!valid) {
            warn("Invalid docID/revID '%.*s' #%.*s in incoming change list",
                 FMTSLICE(docID), FMTSLICE(revID));
        }
        return valid;
    }

}
