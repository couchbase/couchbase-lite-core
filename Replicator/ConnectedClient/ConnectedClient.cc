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
#include "slice_stream.hh"
#include "WebSocketInterface.hh"
#include "c4Internal.hh"
#include "fleece/Mutable.hh"
#include <unordered_map>


#define _options DONT_USE_OPTIONS  // inherited from Worker, but replicator-specific, not used here

namespace litecore::client {
    using namespace std;
    using namespace fleece;
    using namespace litecore::actor;
    using namespace blip;

    alloc_slice ConnectedClient::Delegate::getBlobContents(const C4BlobKey&, C4Error* error) {
        Warn("ConnectedClient's delegate needs to override getBlobContents!");
        *error = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
        return nullslice;
    }

    ConnectedClient::ConnectedClient(websocket::WebSocket* webSocket, Delegate& delegate,
                                     const C4ConnectedClientParameters& params, repl::Options* options)
        : Worker(new Connection(webSocket, AllocedDict(params.optionsDictFleece), {}), nullptr, options, nullptr,
                 "Client", repl::kNotCollectionIndex)
        , _weakConnectionDelegateThis(new WeakHolder<blip::ConnectionDelegate>(this))
        , _delegate(&delegate)
        , _params(params)
        , _activityLevel(kC4Stopped) {
        _importance = 2;
    }

    void ConnectedClient::setStatus(ActivityLevel level) {
        if ( level != _activityLevel ) {
            _activityLevel = level;

            LOCK(_mutex);
            if ( _delegate ) {
                Status status = Worker::status();
                status.level  = _activityLevel;
                _delegate->clientStatusChanged(this, status);
            }
        }
    }

    ConnectedClient::Status ConnectedClient::status() {
        return C4ReplicatorStatus(Worker::status());  //TODO: Thread safety
    }

    void ConnectedClient::start() {
        Assert(_activityLevel == kC4Stopped);
        setStatus(kC4Connecting);
        enqueue("start", &ConnectedClient::_start);
    }

    void ConnectedClient::_start() {
        logInfo("Connecting...");
        connection().start(_weakConnectionDelegateThis);
        registerHandler("getAttachment", &ConnectedClient::handleGetAttachment);
        _selfRetain = this;  // retain myself while the connection is open
    }

    void ConnectedClient::stop() { enqueue("stop", &ConnectedClient::_stop); }

    void ConnectedClient::_stop() { _disconnect(websocket::kCodeNormal, {}); }

    void ConnectedClient::_disconnect(websocket::CloseCode closeCode, slice message) {
        if ( connected() ) {
            logInfo("Disconnecting...");
            connection().close(closeCode, message);
            setStatus(kC4Stopping);
        }
    }

    void ConnectedClient::terminate() {
        LOCK(_mutex);
        _delegate = nullptr;
    }

    ConnectedClient::ActivityLevel ConnectedClient::computeActivityLevel() const { return _activityLevel; }

#pragma mark - BLIP DELEGATE:

    void ConnectedClient::onTLSCertificate(slice certData) {
        LOCK(_mutex);
        if ( _delegate ) _delegate->clientGotTLSCertificate(this, certData);
    }

    void ConnectedClient::onHTTPResponse(int status, const websocket::Headers& headers) {
        enqueue("onHTTPResponse", &ConnectedClient::_onHTTPResponse, status, headers);
    }

    void ConnectedClient::_onHTTPResponse(int status, websocket::Headers headers) {
        logVerbose("Got HTTP response from server, status %d", status);
        {
            LOCK(_mutex);
            if ( _delegate ) _delegate->clientGotHTTPResponse(this, status, headers);
        }

        if ( status == 101 && !headers["Sec-WebSocket-Protocol"_sl] ) {
            gotError(C4Error::make(WebSocketDomain, kWebSocketCloseProtocolError,
                                   "Incompatible replication protocol "
                                   "(missing 'Sec-WebSocket-Protocol' response header)"_sl));
        }
    }

    void ConnectedClient::onConnect() { enqueue("onConnect", &ConnectedClient::_onConnect); }

    void ConnectedClient::_onConnect() {
        logInfo("Connected!");
        if ( _activityLevel != kC4Stopping )  // skip this if stop() already called
            setStatus(kC4Idle);
    }

    void ConnectedClient::onClose(Connection::CloseStatus status, Connection::State state) {
        enqueue("onClose", &ConnectedClient::_onClose, status, state);
    }

    void ConnectedClient::_onClose(Connection::CloseStatus status, Connection::State state) {
        logInfo("Connection closed with %-s %d: \"%.*s\" (state=%d)", status.reasonName(), status.code,
                FMTSLICE(status.message), state);

        bool closedByPeer = (_activityLevel != kC4Stopping);

        _connectionClosed();

        if ( status.isNormal() && closedByPeer ) {
            logInfo("I didn't initiate the close; treating this as code 1001 (GoingAway)");
            status.code    = websocket::kCodeGoingAway;
            status.message = alloc_slice("WebSocket connection closed by peer");
        }

        static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain, NetworkDomain, LiteCoreDomain};

        // If this was an unclean close, set my error property:
        if ( status.reason != websocket::kWebSocketClose || status.code != websocket::kCodeNormal ) {
            int           code = status.code;
            C4ErrorDomain domain;
            if ( status.reason < sizeof(kDomainForReason) / sizeof(C4ErrorDomain) ) {
                domain = kDomainForReason[status.reason];
            } else {
                domain = LiteCoreDomain;
                code   = kC4ErrorRemoteError;
            }
            gotError(C4Error::make(domain, code, status.message));
        }
        setStatus(kC4Stopped);
        {
            LOCK(_mutex);
            if ( _delegate ) _delegate->clientConnectionClosed(this, status);
        }

        _selfRetain = nullptr;  // balances the self-retain in start()
    }

    // This only gets called if none of the registered handlers were triggered.
    void ConnectedClient::onRequestReceived(MessageIn* msg) {
        warn("Received unrecognized BLIP request #%" PRIu64 " with Profile '%.*s', %zu bytes", msg->number(),
             FMTSLICE(msg->profile()), msg->body().size);
        msg->notHandled();
    }

#pragma mark - REQUESTS:

    // Returns the error status of a response (including a NULL response, i.e. disconnection)
    C4Error ConnectedClient::responseError(MessageIn* response) {
        C4Error error;
        if ( !response ) {
            // Disconnected!
            error = Worker::status().error;
            if ( !error ) error = C4Error::make(LiteCoreDomain, kC4ErrorIOError, "network connection lost");
            // TODO: Use a better default error than the one above
        } else if ( response->isError() ) {
            error = blipToC4Error(response->getError());
            if ( error.domain == WebSocketDomain ) {
                switch ( error.code ) {
                    case 404:
                        error.domain = LiteCoreDomain;
                        error.code   = kC4ErrorNotFound;
                        break;
                    case 409:
                        error.domain = LiteCoreDomain;
                        error.code   = kC4ErrorConflict;
                        break;
                }
            }
        } else {
            error = {};
        }
        if ( error ) logError("Connected Client got error response %s", error.description().c_str());
        return error;
    }

    void ConnectedClient::getDoc(slice docID_, slice collectionID_, slice unlessRevID_, bool asFleece,
                                 function<void(Result<DocResponse>)> callback) {
        // Not yet running on Actor thread...
        logInfo("getDoc(\"%.*s\")", FMTSLICE(docID_));
        alloc_slice    docID(docID_);
        MessageBuilder req("getRev");
        req["id"]       = docID;
        req["ifNotRev"] = unlessRevID_;

        sendRequest(req, [=](const MessageProgress& progress) {
            if ( progress.state >= blip::MessageProgress::kComplete ) {
                logInfo("...getDoc got response");
                if ( C4Error err = responseError(progress.reply) ) {
                    callback(err);
                } else {
                    callback(DocResponse{docID, alloc_slice(progress.reply->property("rev")),
                                         processIncomingDoc(docID, progress.reply->body(), asFleece),
                                         progress.reply->boolProperty("deleted")});
                }
            }
        });
    }

    // (This method's code is adapted from IncomingRev::parseAndInsert)
    alloc_slice ConnectedClient::processIncomingDoc(slice docID, alloc_slice jsonData, bool asFleece) {
        if ( !jsonData ) return jsonData;

        bool modified   = false;
        bool tryDecrypt = _params.propertyDecryptor && repl::MayContainPropertiesToDecrypt(jsonData);

        // Convert JSON to Fleece:
        FLError flErr;
        Doc     fleeceDoc = Doc::fromJSON(jsonData, &flErr);
        if ( !fleeceDoc ) C4Error::raise(FleeceDomain, flErr, "Unparseable JSON response from server");
        alloc_slice fleeceData = fleeceDoc.allocedData();
        Dict        root       = fleeceDoc.asDict();

        // Decrypt properties:
        MutableDict decryptedRoot;
        if ( tryDecrypt ) {
            C4Error error;
            decryptedRoot = repl::DecryptDocumentProperties({},  //TODO: Pass collection spec
                                                            docID, root, _params.propertyDecryptor,
                                                            _params.callbackContext, &error);
            if ( decryptedRoot ) {
                root     = decryptedRoot;
                modified = true;
            } else if ( error ) {
                error.raise();
            }
        }

        // Strip out any "_"-prefixed properties like _id, just in case, and also any
        // attachments in _attachments that are redundant with blobs elsewhere in the doc.
        // This also re-encodes, updating fleeceData, if `root` was modified by the decryptor.
        if ( modified || legacy_attachments::hasOldMetaProperties(root) ) {
            fleeceData = legacy_attachments::encodeStrippingOldMetaProperties(root, nullptr);
            if ( !fleeceData )
                C4Error::raise(LiteCoreDomain, kC4ErrorRemoteError, "Invalid legacy attachments received from server");
            //modified = true;
            if ( !asFleece ) jsonData = Doc(fleeceData, kFLTrusted).root().toJSON();
        }

        return asFleece ? fleeceData : jsonData;
    }

    void ConnectedClient::getBlob(C4BlobKey blobKey, bool compress, function<void(Result<alloc_slice>)> callback) {
        // Not yet running on Actor thread...
        auto digest = blobKey.digestString();
        logInfo("getAttachment(<%s>)", digest.c_str());
        MessageBuilder req("getAttachment");
        req["digest"] = digest;
        if ( compress ) req["compress"] = "true";

        sendRequest(req, [=](const MessageProgress& progress) {
            if ( progress.state >= blip::MessageProgress::kComplete ) {
                logInfo("...getAttachment got response");
                if ( C4Error err = responseError(progress.reply) ) callback(err);
                else
                    callback(progress.reply->body());
            }
        });
    }

    void ConnectedClient::putDoc(slice docID_, slice collectionID_, slice revID_, slice parentRevID_,
                                 C4RevisionFlags revisionFlags, slice fleeceData_, function<void(C4Error)> callback) {
        // Not yet running on Actor thread...
        logInfo("putDoc(\"%.*s\", \"%.*s\")", FMTSLICE(docID_), FMTSLICE(revID_));
        MessageBuilder req("putRev");
        req.compressed = true;
        req["id"]      = docID_;
        req["rev"]     = revID_;
        req["history"] = parentRevID_;
        if ( revisionFlags & kRevDeleted ) req["deleted"] = "1";

        if ( fleeceData_.size > 0 ) {
            processOutgoingDoc(docID_, revID_, fleeceData_, req.jsonBody());
        } else {
            req.write("{}");
        }

        sendRequest(req, [=](const MessageProgress& progress) {
            if ( progress.state >= blip::MessageProgress::kComplete ) {
                logInfo("...putDoc got response");
                callback(responseError(progress.reply));
            }
        });
    }

    static bool mayContainBlobs(fleece::slice documentData) noexcept {
        return documentData.find(C4Document::kObjectTypeProperty) && documentData.find(C4Blob::kObjectType_Blob);
    }

    void ConnectedClient::processOutgoingDoc(slice docID, slice revID, slice fleeceData, JSONEncoder& enc) {
        Dict root = Value(FLValue_FromData(fleeceData, kFLUntrusted)).asDict();
        if ( !root )
            C4Error::raise(LiteCoreDomain, kC4ErrorCorruptRevisionData,
                           "Invalid Fleece data passed to ConnectedClient::putDoc");

        // Encrypt any encryptable properties
        MutableDict encryptedRoot;
        if ( repl::MayContainPropertiesToEncrypt(fleeceData) ) {
            logVerbose("Encrypting properties in doc '%.*s'", FMTSLICE(docID));
            C4Error c4err;
            encryptedRoot = repl::EncryptDocumentProperties({},  // TODO: Pass collection spec
                                                            docID, root, _params.propertyEncryptor,
                                                            _params.callbackContext, &c4err);
            if ( encryptedRoot ) root = encryptedRoot;
            else if ( c4err )
                c4err.raise();
        }

        if ( _remoteNeedsLegacyAttachments && mayContainBlobs(fleeceData) ) {
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
        C4Error     error = {};
        try {
            if ( auto blobKey = C4BlobKey::withDigestString(req->property("digest"_sl)) )
                contents = _delegate->getBlobContents(*blobKey, &error);
            else
                error = C4Error::make(WebSocketDomain, 400, "Invalid 'digest' property in request");
        } catch ( ... ) { error = C4Error::fromCurrentException(); }
        if ( !contents ) {
            if ( !error ) error = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
            req->respondWithError(c4ToBLIPError(error));
            return;
        }

        MessageBuilder reply(req);
        reply.compressed = req->boolProperty("compress"_sl);
        reply.write(contents);
        req->respond(reply);
    }

    void ConnectedClient::getAllDocIDs(slice collectionID, slice globPattern, AllDocsReceiver receiver) {
        MessageBuilder req("allDocs");
        if ( !globPattern.empty() ) req["idPattern"] = globPattern;
        sendRequest(req, [this, receiver](const MessageProgress& progress) {
            if ( progress.state >= blip::MessageProgress::kComplete ) {
                logInfo("...allDocs got response");
                C4Error err = responseError(progress.reply);
                if ( !err ) {
                    if ( !receiveAllDocs(progress.reply, receiver) )
                        err = C4Error::make(LiteCoreDomain, kC4ErrorRemoteError, "Invalid allDocs response");
                }
                // Final call to receiver:
                receiver({}, err ? &err : nullptr);
            }
        });
        //OPT: If we stream the response we can call the receiver function on results as they arrive.
    }

    bool ConnectedClient::receiveAllDocs(blip::MessageIn* response, const AllDocsReceiver& receiver) {
        Array body = response->JSONBody().asArray();
        if ( !body ) return false;
        if ( body.empty() ) return true;
        vector<slice> docIDs;
        docIDs.reserve(body.count());
        for ( Array::iterator i(body); i; ++i ) {
            slice docID = i->asString();
            if ( !docID ) return false;
            docIDs.push_back(docID);
        }
        receiver(docIDs, nullptr);
        return true;
    }

    void ConnectedClient::observeCollection(slice collectionID, CollectionObserver callback) {
        enqueue("observeCollection", &ConnectedClient::_observeCollection, alloc_slice(collectionID),
                std::move(callback));
    }

    void ConnectedClient::_observeCollection(alloc_slice collectionID, CollectionObserver callback) {
        logInfo("observeCollection(%.*s)", FMTSLICE(collectionID));

        bool observe      = !!callback;
        bool sameSubState = (observe == !!_observer);
        _observer         = std::move(callback);
        if ( sameSubState ) return;

        MessageBuilder req;
        if ( observe ) {
            if ( !_registeredChangesHandler ) {
                registerHandler("changes", &ConnectedClient::handleChanges);
                _registeredChangesHandler = true;
            }
            req.setProfile("subChanges");
            req["future"]     = true;
            req["continuous"] = true;
        } else {
            req.setProfile("unsubChanges");
        }

        sendRequest(req, [this, callback](const MessageProgress& progress) {
            if ( progress.state >= blip::MessageProgress::kComplete ) {
                logInfo("...observeCollection got response");
                if ( C4Error err = responseError(progress.reply) ) {
                    _observer = nullptr;
                    callback({}, &err);  // Request failed
                }
            }
        });
    }

    void ConnectedClient::handleChanges(Retained<blip::MessageIn> req) {
        // The code below is adapted from RevFinder::handleChangesNow and RevFinder::findRevs.
        auto inChanges = req->JSONBody().asArray();
        if ( !inChanges && req->body() != "null"_sl ) {
            warn("Invalid body of 'changes' message");
            req->respondWithError(400, "Invalid JSON body"_sl);
            return;
        }

        // "changes" expects a response with an array of which items we want "rev" messages for.
        // We don't actually want any. An empty array will indicate that.
        if ( !req->noReply() ) {
            MessageBuilder response(req);
            auto&          enc = response.jsonBody();
            enc.beginArray();
            enc.endArray();
            req->respond(response);
        }

        if ( _observer && !inChanges.empty() ) {
            logInfo("Received %u doc changes from server", inChanges.count());
            // Convert the JSON change list into a vector:
            vector<C4CollectionObserver::Change> outChanges;
            outChanges.reserve(inChanges.count());
            for ( auto item : inChanges ) {
                // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
                auto  inChange = item.asArray();
                slice docID    = inChange[1].asString();
                slice revID    = inChange[2].asString();
                if ( validateDocAndRevID(docID, revID) ) {
                    auto& outChange    = outChanges.emplace_back();
                    outChange.sequence = C4SequenceNumber{inChange[0].asUnsigned()};
                    outChange.docID    = docID;
                    outChange.revID    = revID;
                    outChange.flags    = 0;
                    int64_t deletion   = inChange[3].asInt();
                    outChange.bodySize = fleece::narrow_cast<uint32_t>(inChange[4].asUnsigned());

                    // In SG 2.x "deletion" is a boolean flag, 0=normal, 1=deleted.
                    // SG 3.x adds 2=revoked, 3=revoked+deleted, 4=removal (from channel)
                    if ( deletion & 0b001 ) outChange.flags |= kRevDeleted;
                    if ( deletion & 0b110 ) outChange.flags |= kRevPurged;
                }
            }

            // Finally call the observer callback:
            try {
                _observer(outChanges, nullptr);
            } catch ( ... ) {
                logError("ConnectedClient observer threw exception: %s",
                         C4Error::fromCurrentException().description().c_str());
            }
        }
    }

    bool ConnectedClient::validateDocAndRevID(slice docID, slice revID) {
        bool valid;
        if ( !C4Document::isValidDocID(docID) ) valid = false;
        else if ( _remoteUsesVersionVectors )
            valid = revID.findByte('@') && !revID.findByte('*');  // require absolute form
        else
            valid = revID.findByte('-');
        if ( !valid ) {
            warn("Invalid docID/revID '%.*s' #%.*s in incoming change list", FMTSLICE(docID), FMTSLICE(revID));
        }
        return valid;
    }

    void ConnectedClient::query(slice name, fleece::Dict parameters, bool asFleece, QueryReceiver receiver) {
        MessageBuilder req("query");
        if ( name.hasPrefix("SELECT ") || name.hasPrefix("select ") || name.hasPrefix("{") ) req["src"] = name;
        else
            req["name"] = name;
        if ( parameters ) {
            req.jsonBody().writeValue(parameters);
        } else {
            req.jsonBody().beginDict();
            req.jsonBody().endDict();
        }

        sendRequest(req, [=](const MessageProgress& progress) {
            if ( progress.state >= blip::MessageProgress::kComplete ) {
                logInfo("...query got response");
                C4Error err = responseError(progress.reply);
                if ( !err ) {
                    if ( !receiveQueryRows(progress.reply, receiver, asFleece) )
                        err = C4Error::make(LiteCoreDomain, kC4ErrorRemoteError, "Couldn't parse server's response");
                }
                receiver(nullslice, nullptr, err ? &err : nullptr);
            }
        });
        //OPT: If we stream the response we can call the receiver function on results as they arrive.
    }


#if DEBUG
    static constexpr bool kCheckJSON = true;
#else
    static constexpr bool kCheckJSON = false;
#endif


    // not currently used; keeping it in case we decide to change the response format to lines-of-JSON
    bool ConnectedClient::receiveQueryRows(blip::MessageIn* response, const QueryReceiver& receiver, bool asFleece) {
        slice_istream body(response->body());
        while ( !body.eof() ) {
            // Get next line of JSON, up to a newline:
            slice rowData = body.readToDelimiterOrEnd("\n");
            if ( !rowData.empty() ) {
                Dict rowDict;
                Doc  doc;
                if ( asFleece || kCheckJSON ) {
                    doc     = Doc::fromJSON(rowData);
                    rowDict = doc.asDict();
                    if ( !rowDict ) return false;
                    if ( !asFleece ) rowDict = nullptr;
                }
                receiver(rowData, rowDict, nullptr);
            }
        }
        return true;
    }

}  // namespace litecore::client
