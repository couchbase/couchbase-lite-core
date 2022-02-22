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
#include "c4SocketTypes.h"
#include "Headers.hh"
#include "MessageBuilder.hh"
#include "WebSocketInterface.hh"

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
            _delegate->clientStatusChanged(this, status);
        }
    }


    void ConnectedClient::start() {
        BEGIN_ASYNC()
        logInfo("Connecting...");
        Assert(_status == kC4Stopped);
        setStatus(kC4Connecting);
        connection().start();
        _selfRetain = this;     // retain myself while the connection is open
        END_ASYNC();
    }

    void ConnectedClient::stop() {
        BEGIN_ASYNC()
        _disconnect(websocket::kCodeNormal, {});
        END_ASYNC();
    }


    void ConnectedClient::_disconnect(websocket::CloseCode closeCode, slice message) {
        if (connected()) {
            logInfo("Disconnecting...");
            connection().close(closeCode, message);
            setStatus(kC4Stopping);
        }
    }


#pragma mark - BLIP DELEGATE:


    void ConnectedClient::onTLSCertificate(slice certData) {
        if (_delegate)
            _delegate->clientGotTLSCertificate(this, certData);
    }


    void ConnectedClient::onHTTPResponse(int status, const websocket::Headers &headers) {
        BEGIN_ASYNC()
        logVerbose("Got HTTP response from server, status %d", status);
        if (_delegate)
            _delegate->clientGotHTTPResponse(this, status, headers);
        if (status == 101 && !headers["Sec-WebSocket-Protocol"_sl]) {
            gotError(C4Error::make(WebSocketDomain, kWebSocketCloseProtocolError,
                                   "Incompatible replication protocol "
                                   "(missing 'Sec-WebSocket-Protocol' response header)"_sl));
        }
        END_ASYNC()
    }


    void ConnectedClient::onConnect() {
        BEGIN_ASYNC()
        logInfo("Connected!");
        if (_status != kC4Stopping)       // skip this if stop() already called
            setStatus(kC4Idle);
        END_ASYNC()
    }


    void ConnectedClient::onClose(Connection::CloseStatus status, Connection::State state) {
        BEGIN_ASYNC()
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

        if (_delegate)
            _delegate->clientConnectionClosed(this, status);

        _selfRetain = nullptr;  // balances the self-retain in start()
        END_ASYNC()
    }


    // This only gets called if none of the registered handlers were triggered.
    void ConnectedClient::onRequestReceived(MessageIn *msg) {
        warn("Received unrecognized BLIP request #%" PRIu64 " with Profile '%.*s', %zu bytes",
             msg->number(), FMTSLICE(msg->property("Profile"_sl)), msg->body().size);
        msg->notHandled();
    }


#pragma mark - REQUESTS:


    // Returns the error status of a response (including a NULL response, i.e. disconnection)
    C4Error ConnectedClient::responseError(MessageIn *response) {
        if (!response) {
            // Disconnected!
            return status().error ? status().error : C4Error::make(LiteCoreDomain,
                                                                   kC4ErrorIOError,
                                                                   "network connection lost");
            // TODO: Use a better default error than the one above
        } else if (response->isError()) {
            return blipToC4Error(response->getError());
        } else {
            return {};
        }
    }


    Async<DocResponseOrError> ConnectedClient::getDoc(alloc_slice docID,
                                                      alloc_slice collectionID,
                                                      alloc_slice unlessRevID,
                                                      bool asFleece)
    {
        BEGIN_ASYNC_RETURNING(DocResponseOrError)
        logInfo("getDoc(\"%.*s\")", FMTSLICE(docID));
        MessageBuilder req("getRev");
        req["id"] = docID;
        req["ifNotRev"] = unlessRevID;

        AWAIT(Retained<MessageIn>, response, sendAsyncRequest(req));
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
                return C4Error::make(FleeceDomain, flErr);
        }
        return docResponse;
        END_ASYNC()
    }


    Async<BlobOrError> ConnectedClient::getBlob(C4BlobKey blobKey,
                                                bool compress)
    {
        BEGIN_ASYNC_RETURNING(BlobOrError)
        auto digest = blobKey.digestString();
        logInfo("getAttachment(<%s>)", digest.c_str());
        MessageBuilder req("getAttachment");
        req["digest"] = digest;
        if (compress)
            req["compress"] = "true";

        AWAIT(Retained<MessageIn>, response, sendAsyncRequest(req));
        logInfo("...getAttachment got response");

        if (C4Error err = responseError(response))
            return err;
        return response->body();
        END_ASYNC()
    }


    Async<C4Error> ConnectedClient::putDoc(alloc_slice docID,
                                           alloc_slice collectionID,
                                           alloc_slice revID,
                                           alloc_slice parentRevID,
                                           C4RevisionFlags revisionFlags,
                                           alloc_slice fleeceData)
    {
        BEGIN_ASYNC_RETURNING(C4Error)
        logInfo("putDoc(\"%.*s\", \"%.*s\")", FMTSLICE(docID), FMTSLICE(revID));
        MessageBuilder req("putRev");
        req.compressed = true;
        req["id"] = docID;
        req["rev"] = revID;
        req["history"] = parentRevID;
        if (revisionFlags & kRevDeleted)
            req["deleted"] = "1";

        if (fleeceData.size > 0) {
            // TODO: Encryption!!
            // TODO: Convert blobs to legacy attachments
            req.jsonBody().writeValue(Doc(fleeceData, kFLTrusted).asDict());
        } else {
            req.write("{}");
        }

        AWAIT(Retained<MessageIn>, response, sendAsyncRequest(req));
        logInfo("...putDoc got response");

        return responseError(response);
        END_ASYNC()
    }

}
