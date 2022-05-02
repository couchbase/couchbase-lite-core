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
#include "c4BlobStore.hh"
#include "c4Document.hh"
#include "c4SocketTypes.h"
#include "Headers.hh"
#include "LegacyAttachments.hh"
#include "MessageBuilder.hh"
#include "NumConversion.hh"
#include "PropertyEncryption.hh"
#include "WebSocketInterface.hh"
#include "c4Internal.hh"
#include "fleece/Mutable.hh"


#define _options DONT_USE_OPTIONS   // inherited from Worker, but replicator-specific, not used here

namespace litecore::client {
    using namespace std;
    using namespace fleece;
    using namespace litecore::actor;
    using namespace blip;


    alloc_slice ConnectedClient::Delegate::getBlobContents(const C4BlobKey &, C4Error *error) {
        Warn("ConnectedClient's delegate needs to override getBlobContents!");
        *error = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
        return nullslice;
    }



    ConnectedClient::ConnectedClient(websocket::WebSocket* webSocket,
                                     Delegate& delegate,
                                     const C4ConnectedClientParameters &params)
    :Worker(new Connection(webSocket, AllocedDict(params.optionsDictFleece), *this),
            nullptr, nullptr, nullptr, "Client")
    ,_delegate(&delegate)
    ,_params(params)
    ,_activityLevel(kC4Stopped)
    {
        _importance = 2;
    }


    void ConnectedClient::setStatus(ActivityLevel status) {
        if (status != _activityLevel) {
            _activityLevel = status;
            
            LOCK(_mutex);
            if (_delegate)
                _delegate->clientStatusChanged(this, status);
        }
    }


    Async<ConnectedClient::Status> ConnectedClient::status() {
        return asCurrentActor([this]() {return C4ReplicatorStatus(Worker::status());});
    }


    void ConnectedClient::start() {
        Assert(_activityLevel == kC4Stopped);
        setStatus(kC4Connecting);
        asCurrentActor([=] {
            logInfo("Connecting...");
            connection().start();
            registerHandler("getAttachment", &ConnectedClient::handleGetAttachment);
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


    ConnectedClient::ActivityLevel ConnectedClient::computeActivityLevel() const {
        return _activityLevel;
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
        if (_activityLevel != kC4Stopping)       // skip this if stop() already called
            setStatus(kC4Idle);
        });
    }


    void ConnectedClient::onClose(Connection::CloseStatus status, Connection::State state) {
        asCurrentActor([=]() mutable {
            logInfo("Connection closed with %-s %d: \"%.*s\" (state=%d)",
                    status.reasonName(), status.code, FMTSLICE(status.message), state);

            bool closedByPeer = (_activityLevel != kC4Stopping);
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
            error = Worker::status().error;
            if (!error)
                error = C4Error::make(LiteCoreDomain, kC4ErrorIOError, "network connection lost");
            // TODO: Use a better default error than the one above
        } else if (response->isError()) {
            error = blipToC4Error(response->getError());
            if (error.domain == WebSocketDomain) {
                switch (error.code) {
                    case 404: error.domain = LiteCoreDomain; error.code = kC4ErrorNotFound; break;
                    case 409: error.domain = LiteCoreDomain; error.code = kC4ErrorConflict; break;
                }
            }
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

                return DocResponse {
                    docID,
                    alloc_slice(response->property("rev")),
                    processIncomingDoc(docID, response->body(), asFleece),
                    response->boolProperty("deleted")
                };
            });
    }


    // (This method's code is adapted from IncomingRev::parseAndInsert)
    alloc_slice ConnectedClient::processIncomingDoc(slice docID,
                                                    alloc_slice jsonData,
                                                    bool asFleece)
    {
        if (!jsonData)
            return jsonData;

        bool modified = false;
        bool tryDecrypt = _params.propertyDecryptor && repl::MayContainPropertiesToDecrypt(jsonData);

        // Convert JSON to Fleece:
        FLError flErr;
        Doc fleeceDoc = Doc::fromJSON(jsonData, &flErr);
        if (!fleeceDoc)
            C4Error::raise(FleeceDomain, flErr, "Unparseable JSON response from server");
        alloc_slice fleeceData = fleeceDoc.allocedData();
        Dict root = fleeceDoc.asDict();

        // Decrypt properties:
        MutableDict decryptedRoot;
        if (tryDecrypt) {
            C4Error error;
            decryptedRoot = repl::DecryptDocumentProperties(docID,
                                                            root,
                                                            _params.propertyDecryptor,
                                                            _params.callbackContext,
                                                            &error);
            if (decryptedRoot) {
                root = decryptedRoot;
                modified = true;
            } else if (error) {
                error.raise();
            }
        }

        // Strip out any "_"-prefixed properties like _id, just in case, and also any
        // attachments in _attachments that are redundant with blobs elsewhere in the doc.
        // This also re-encodes, updating fleeceData, if `root` was modified by the decryptor.
        if (modified || legacy_attachments::hasOldMetaProperties(root)) {
            fleeceData = legacy_attachments::encodeStrippingOldMetaProperties(root, nullptr);
            if (!fleeceData)
                C4Error::raise(LiteCoreDomain, kC4ErrorRemoteError,
                               "Invalid legacy attachments received from server");
            //modified = true;
            if (!asFleece)
                jsonData = Doc(fleeceData, kFLTrusted).root().toJSON();
        }

        return asFleece ? fleeceData : jsonData;
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
            processOutgoingDoc(docID_, revID_, fleeceData_, req.jsonBody());
        } else {
            req.write("{}");
        }

        return sendAsyncRequest(req)
            .then([=](Retained<blip::MessageIn> response) -> Async<void> {
                logInfo("...putDoc got response");
                return Async<void>(responseError(response));
            });
    }


    static inline bool MayContainBlobs(fleece::slice documentData) noexcept {
        return documentData.find(C4Document::kObjectTypeProperty)
            && documentData.find(C4Blob::kObjectType_Blob);
    }


    void ConnectedClient::processOutgoingDoc(slice docID, slice revID,
                                             slice fleeceData,
                                             JSONEncoder &enc)
    {
        Dict root = Value(FLValue_FromData(fleeceData, kFLUntrusted)).asDict();
        if (!root)
            C4Error::raise(LiteCoreDomain, kC4ErrorCorruptRevisionData,
                           "Invalid Fleece data passed to ConnectedClient::putDoc");

        // Encrypt any encryptable properties
        MutableDict encryptedRoot;
        if (repl::MayContainPropertiesToEncrypt(fleeceData)) {
            logVerbose("Encrypting properties in doc '%.*s'", FMTSLICE(docID));
            C4Error c4err;
            encryptedRoot = repl::EncryptDocumentProperties(docID, root,
                                                            _params.propertyEncryptor,
                                                            _params.callbackContext,
                                                            &c4err);
            if (encryptedRoot)
                root = encryptedRoot;
            else if (c4err)
                c4err.raise();
        }

        if (_remoteNeedsLegacyAttachments && MayContainBlobs(fleeceData)) {
            // Create shadow copies of blobs, in `_attachments`:
            int revpos = C4Document::getRevIDGeneration(revID);
            legacy_attachments::encodeRevWithLegacyAttachments(enc, root, revpos);
        } else {
            enc.writeValue(root);
        }
    }


    void ConnectedClient::handleGetAttachment(Retained<MessageIn> req) {
        // Pass the buck to the delegate:
        alloc_slice contents;
        C4Error error = {};
        try {
            if (auto blobKey = C4BlobKey::withDigestString(req->property("digest"_sl)))
                contents = _delegate->getBlobContents(*blobKey, &error);
            else
                error = C4Error::make(WebSocketDomain, 400, "Invalid 'digest' property in request");
        } catch (...) {
            error = C4Error::fromCurrentException();
        }
        if (!contents) {
            if (!error)
                error = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
            req->respondWithError(c4ToBLIPError(error));
            return;
        }

        MessageBuilder reply(req);
        reply.compressed = req->boolProperty("compress"_sl);
        reply.write(contents);
        req->respond(reply);
    }


    void ConnectedClient::getAllDocIDs(slice collectionID,
                                       slice globPattern,
                                       AllDocsReceiver receiver)
    {
        MessageBuilder req("allDocs");
        if (!globPattern.empty())
            req["idPattern"] = globPattern;
        sendAsyncRequest(req)
            .then([=](Retained<blip::MessageIn> response) {
                logInfo("...allDocs got response");
                C4Error err = responseError(response);
                if (!err) {
                    if (!receiveAllDocs(response, receiver))
                        err = C4Error::make(LiteCoreDomain, kC4ErrorRemoteError,
                                            "Invalid allDocs response");
                }
                // Final call to receiver:
                receiver({}, err ? &err : nullptr);

            }).onError([=](C4Error err) {
                logInfo("...allDocs got error");
                receiver({}, &err);
            });
        //OPT: If we stream the response we can call the receiver function on results as they arrive.
    }


    bool ConnectedClient::receiveAllDocs(blip::MessageIn *response, const AllDocsReceiver &receiver) {
        Array body = response->JSONBody().asArray();
        if (!body)
            return false;
        if (body.empty())
            return true;
        vector<slice> docIDs;
        docIDs.reserve(body.count());
        for (Array::iterator i(body); i; ++i) {
            slice docID = i->asString();
            if (!docID)
                return false;
            docIDs.push_back(docID);
        }
        receiver(docIDs, nullptr);
        return true;
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
                req["future"]     = true;
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


    void ConnectedClient::query(slice name, fleece::Dict parameters, QueryReceiver receiver) {
        MessageBuilder req("query");
        if (name.hasPrefix("SELECT ") || name.hasPrefix("select ") || name.hasPrefix("{"))
            req["src"] = name;
        else
            req["name"] = name;
        if (parameters) {
            req.jsonBody().writeValue(parameters);
        } else {
            req.jsonBody().beginDict();
            req.jsonBody().endDict();
        }
        sendAsyncRequest(req)
            .then([=](Retained<blip::MessageIn> response) {
                logInfo("...query got response");
                C4Error err = responseError(response);
                if (!err) {
                    if (!receiveQueryRows(response, receiver))
                        err = C4Error::make(LiteCoreDomain, kC4ErrorRemoteError,
                                            "Invalid query response");
                }
                // Final call to receiver:
                receiver(nullptr, err ? &err : nullptr);

            }).onError([=](C4Error err) {
                logInfo("...query got error");
                receiver(nullptr, &err);
            });
        //OPT: If we stream the response we can call the receiver function on results as they arrive.
    }


    bool ConnectedClient::receiveQueryRows(blip::MessageIn *response, const QueryReceiver &receiver) {
        Array rows = response->JSONBody().asArray();
        if (!rows)
            return false;
        for (Array::iterator i(rows); i; ++i) {
            Array row = i->asArray();
            if (!row)
                return false;
            receiver(row, nullptr);
        }
        return true;
    }


    // not currently used; keeping it in case we decide to change the response format to lines-of-JSON
    bool ConnectedClient::receiveMultiLineQueryRows(blip::MessageIn *response, const QueryReceiver &receiver) {
        slice body = response->body();
        while (!body.empty()) {
            // Get next line of JSON, up to a newline:
            slice rowData;
            if (const void *nl = body.findByte('\n')) {
                rowData = body.upTo(nl);
                body.setStart(offsetby(nl, 1));
            } else {
                rowData = body;
                body = nullslice;
            }
            Doc rowDoc = Doc::fromJSON(rowData);
            if (Array row = rowDoc.asArray()) {
                // Pass row to receiver:
                receiver(row, nullptr);
            } else {
                return false;
            }
        }
        return true;
    }

}
