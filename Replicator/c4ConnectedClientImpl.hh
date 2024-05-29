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
#include "CollectionImpl.hh"
#include "Headers.hh"
#include "Replicator.hh"
#include "RevTree.hh"
#include "TreeDocument.hh"
#include "c4ConnectedClient.hh"
#include "c4Socket+Internal.hh"
#include "c4Internal.hh"

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
        C4ConnectedClientImpl(C4Database* db, const C4ConnectedClientParameters& params)
            : _db(db), _onStatusChanged(params.onStatusChanged), _callbackContext(params.callbackContext) {
            if ( params.socketFactory ) {
                // Keep a copy of the C4SocketFactory struct in case original is invalidated:
                _socketFactory = *params.socketFactory;
            }

            auto options   = make_retained<repl::Options>(kC4Passive, kC4Passive);
            auto webSocket = repl::CreateWebSocket(effectiveURL(params.url), socketOptions(params), nullptr,
                                                   (_socketFactory ? &*_socketFactory : nullptr));
            _client        = new ConnectedClient(db, webSocket, *this, params, options);
            _client->start();
        }

        virtual void start() override {
            LOCK(_mutex);
            _client->start();
        }

        virtual void stop() override {
            LOCK(_mutex);
            _client->stop();
        }

        C4ConnectedClientStatus getStatus() const override { return _client->status(); }

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

        void getDoc(C4CollectionSpec const& collection, slice docID, slice unlessRevID, bool asFleece,
                    C4ConnectedClientGetDocumentCallback callback, void* context) override {
            _client->getDoc(collection, docID, unlessRevID, asFleece, [=](Result<client::DocResponse> r) {
                C4DocResponse doc = {};
                if ( r.ok() ) {
                    auto& v = r.value();
                    doc     = {v.docID, v.revID, v.body, v.deleted};
                    callback(this, &doc, nullptr, context);
                } else {
                    C4Error error = r.error();
                    callback(this, nullptr, &error, context);
                }
            });
        }

        void putDoc(C4CollectionSpec const& collection, slice docID, slice parentRevID, C4RevisionFlags revisionFlags,
                    slice fleeceData, C4ConnectedClientUpdateDocumentCallback callback, void* context) override {
            // Ask the doc factory to generate a revID for this new revision.
            C4Collection* coll = _db->getCollection(collection);
            if ( !coll ) error::_throw(error::NotFound, "no such collection in local database");
            DocumentFactory* fac = asInternal(coll)->documentFactory();
            alloc_slice      generatedRev =
                    fac->generateDocRevID(fleeceData, parentRevID, (revisionFlags & kRevDeleted) != 0);

            _client->putDoc(collection, docID, revid(generatedRev).expanded(), parentRevID, revisionFlags, fleeceData,
                            [=](Result<void> r) {
                                if ( r.ok() ) {
                                    auto revID = revid(generatedRev).expanded();
                                    callback(this, revID, nullptr, context);
                                } else {
                                    C4Error error = r.error();
                                    callback(this, {}, &error, context);
                                }
                            });
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
            opts.setProperty(kC4SocketOptionWSProtocols, repl::Replicator::ProtocolName().c_str());
            return opts.properties.data();
        }

        mutable std::mutex                     _mutex;
        Retained<C4Database>                   _db;
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
