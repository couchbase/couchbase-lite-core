//
// ConnectedClientImpl.hh
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "c4Base.h"
#include "ConnectedClient.hh"
#include "c4ConnectedClient.hh"
#include "c4Socket+Internal.hh"
#include "c4Internal.hh"
#include "Headers.hh"
#include "Replicator.hh"
#include "RevTree.hh"
#include "TreeDocument.hh"

#ifdef COUCHBASE_ENTERPRISE
#    include "c4Certificate.hh"
#endif

namespace litecore::client {

    using namespace litecore::websocket;
    using namespace litecore::actor;
    using namespace std;

    struct C4ConnectedClientImpl final
        : public C4ConnectedClient
        , public ConnectedClient::Delegate {
      public:
        C4ConnectedClientImpl(const C4ConnectedClientParameters& params)
            : _onStatusChanged(params.onStatusChanged), _callbackContext(params.callbackContext) {
            if ( params.socketFactory ) {
                // Keep a copy of the C4SocketFactory struct in case original is invalidated:
                _socketFactory = *params.socketFactory;
            }

            auto webSocket = repl::CreateWebSocket(effectiveURL(params.url), socketOptions(params), nullptr,
                                                   (_socketFactory ? &*_socketFactory : nullptr));
            _client        = new ConnectedClient(webSocket, *this, params);
            _client->start();
        }

        Async<C4ConnectedClientStatus> getStatus() const override { return _client->status(); }

        alloc_slice getResponseHeaders() const noexcept override { return _responseHeaders; }

#ifdef COUCHBASE_ENTERPRISE
        C4Cert* getPeerTLSCertificate() const override {
            LOCK(_mutex);
            if ( !_peerTLSCertificate && _peerTLSCertificateData ) {
                _peerTLSCertificate     = C4Cert::fromData(_peerTLSCertificateData);
                _peerTLSCertificateData = nullptr;
            }
            return _peerTLSCertificate;
        }
#endif


#pragma mark - ConnectedClient Delegate

      protected:
        virtual void clientGotHTTPResponse(ConnectedClient* client, int status,
                                           const websocket::Headers& headers) override {
            _responseHeaders = headers.encode();
        }

        virtual void clientGotTLSCertificate(ConnectedClient* client, slice certData) override {
#ifdef COUCHBASE_ENTERPRISE
            LOCK(_mutex);
            _peerTLSCertificateData = certData;
            _peerTLSCertificate     = nullptr;
#endif
        }

        virtual void clientStatusChanged(ConnectedClient* client, const ConnectedClient::Status& status) override {
            if ( _onStatusChanged ) _onStatusChanged(this, status, _callbackContext);
        }

        virtual void clientConnectionClosed(ConnectedClient*, const CloseStatus& status) override {
            // (do we need to do anything here?)
        }

#pragma mark -

        Async<DocResponse> getDoc(slice docID, slice collectionID, slice unlessRevID, bool asFleece) override {
            return _client->getDoc(docID, collectionID, unlessRevID, asFleece).then([](auto a) -> DocResponse {
                return {a.docID, a.revID, a.body, a.deleted};
            });
        }

        Async<std::string> putDoc(slice docID, slice collectionID, slice parentRevisionID, C4RevisionFlags flags,
                                  slice fleeceData) override {
            bool        deletion     = (flags & kRevDeleted) != 0;
            revidBuffer generatedRev = TreeDocumentFactory::generateDocRevID(fleeceData, parentRevisionID, deletion);
            auto        provider     = Async<std::string>::makeProvider();
            _client->putDoc(docID, collectionID, revid(generatedRev).expanded(), parentRevisionID, flags, fleeceData)
                    .then([=](Result<void> i) {
                        if ( i.ok() ) {
                            auto revID = revid(generatedRev).expanded();
                            provider->setResult(revID.asString());
                        } else
                            provider->setError(i.error());
                    });
            return provider->asyncValue();
        }

        void getAllDocIDs(slice collectionID, slice pattern, AllDocsReceiver callback) override {
            _client->getAllDocIDs(collectionID, pattern, callback);
        }

        void query(slice name, FLDict params, bool asFleece, QueryReceiver rcvr) override {
            _client->query(name, params, asFleece, rcvr);
        }

        virtual void start() override {
            LOCK(_mutex);
            _client->start();
        }

        virtual void stop() override {
            LOCK(_mutex);
            _client->stop();
        }

      private:
        ~C4ConnectedClientImpl() {
            if ( _client ) _client->terminate();

            _client = nullptr;
        }

        alloc_slice effectiveURL(slice url) {
            string newPath(url);
            if ( !url.hasSuffix("/") ) newPath += "/";
            newPath += "_blipsync";
            return alloc_slice(newPath);
        }

        alloc_slice socketOptions(const C4ConnectedClientParameters& params) {
            // Use a temporary repl::Options object,
            // because it has the handy ability to add properties to an existing Fleece dict.
            repl::Options opts(kC4Disabled, kC4Disabled, params.optionsDictFleece);
            opts.setProperty(kC4SocketOptionWSProtocols, repl::Replicator::protocolName().c_str());
            return opts.properties.data();
        }

        mutable std::mutex                     _mutex;
        Retained<ConnectedClient>              _client;
        optional<C4SocketFactory>              _socketFactory;
        C4ConnectedClientStatusChangedCallback _onStatusChanged;
        void*                                  _callbackContext;
        alloc_slice                            _responseHeaders;
#ifdef COUCHBASE_ENTERPRISE
        mutable alloc_slice      _peerTLSCertificateData;
        mutable Retained<C4Cert> _peerTLSCertificate;
#endif
    };
}  // namespace litecore::client
