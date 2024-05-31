//
// c4ConnectedClient.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4ConnectedClient.hh"
#include "ConnectedClient.hh"
#include "CollectionImpl.hh"
#include "Headers.hh"
#include "Replicator.hh"         // for static Replicator::ProtocolName()
#include "c4Socket+Internal.hh"  // for CreateWebSocket()

#ifdef COUCHBASE_ENTERPRISE
#    include "c4Certificate.hh"
#endif

namespace litecore::client {
    using namespace std;

    struct C4ConnectedClientImpl final
        : public C4ConnectedClient
        , public ConnectedClient::Delegate {
      public:
        C4ConnectedClientImpl(C4Database* db, const C4ConnectedClientParameters& params)
            : _db(db)
            , _onStatusChanged(params.onStatusChanged)
            , _blobProvider(params.blobProvider)
            , _callbackContext(params.callbackContext) {
            if ( params.socketFactory ) {
                // Keep a copy of the C4SocketFactory struct in case original is invalidated:
                _socketFactory = *params.socketFactory;
            }

            auto options   = make_retained<repl::Options>(kC4Passive, kC4Passive);
            auto webSocket = repl::CreateWebSocket(effectiveURL(params.url), socketOptions(params), nullptr,
                                                   (_socketFactory ? &*_socketFactory : nullptr));

            _client = new ConnectedClient(db, webSocket, *this, params, options);
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

        alloc_slice getResponseHeaders() const noexcept override { return _client->responseHeaders(); }

#ifdef COUCHBASE_ENTERPRISE
        C4Cert* getPeerTLSCertificate() const override {
            LOCK(_mutex);
            if ( !_peerTLSCertificate ) {
                if ( alloc_slice certData = _client->peerTLSCertificateData() )
                    _peerTLSCertificate = C4Cert::fromData(certData);
            }
            return _peerTLSCertificate;
        }
#endif


#pragma mark - ConnectedClient Delegate

        virtual void clientStatusChanged(ConnectedClient* client, const ConnectedClient::Status& status) override {
            if ( _onStatusChanged ) _onStatusChanged(this, status, _callbackContext);
        }

        virtual void clientConnectionClosed(ConnectedClient*, const websocket::CloseStatus& status) override {
            // (do we need to do anything here?)
        }

        alloc_slice getBlobContents(const C4BlobKey& blobKey, C4Error* C4NULLABLE error) override {
            if ( _blobProvider ) return _blobProvider(this, blobKey, error, _callbackContext);
            else
                return {};
        }

#pragma mark - CRUD:

        void getDoc(C4CollectionSpec const& collection, slice docID, slice unlessRevID, bool asFleece,
                    C4ConnectedClientGetDocumentCallback callback) override {
            _client->getDoc(collection, docID, unlessRevID, asFleece, [=](Result<client::DocResponse> r) {
                if ( r.ok() ) {
                    auto&         v = r.value();
                    C4DocResponse doc{v.docID, v.revID, v.body, v.deleted};
                    callback(this, &doc, nullptr, _callbackContext);
                } else {
                    C4Error error = r.error();
                    callback(this, nullptr, &error, _callbackContext);
                }
            });
        }

        void putDoc(C4CollectionSpec const& collection, slice docID, slice revID, slice parentRevID, C4RevisionFlags revisionFlags,
                    slice fleeceData, C4ConnectedClientUpdateDocumentCallback callback) override {
            alloc_slice newRevID;
            if (revID)
                newRevID = revID;
            else
                newRevID = generateRevID(collection, parentRevID, revisionFlags, fleeceData);
            
            _client->putDoc(collection, docID, newRevID, parentRevID, revisionFlags, fleeceData, [=](Result<void> r) {
                if ( r.ok() ) {
                    callback(this, C4HeapSlice(newRevID), nullptr, _callbackContext);
                } else {
                    C4Error error = r.error();
                    callback(this, {}, &error, _callbackContext);
                }
            });
        }

      private:
        ~C4ConnectedClientImpl() {
            if ( _client ) _client->terminate();
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

        alloc_slice generateRevID(C4CollectionSpec const& collection, slice parentRevID, C4RevisionFlags revisionFlags,
                                  slice fleeceData) {
            // Get the DocumentFactory instance:
            C4Collection* coll = _db->getCollection(collection);
            if ( !coll ) error::_throw(error::NotFound, "no such collection in local database");
            DocumentFactory* fac = asInternal(coll)->documentFactory();
            // Ask it to generate a revID:
            alloc_slice revID = fac->generateDocRevID(fleeceData, parentRevID, (revisionFlags & kRevDeleted) != 0);
            revID             = revid(revID).expanded();  // convert to ASCII
            return _db->getRevIDGlobalForm(revID);        // convert to global form, if it's a VV
        }

        mutable std::mutex                     _mutex;
        Retained<C4Database>                   _db;  // Local DB; only used to generate revIDs
        Retained<ConnectedClient>              _client;
        optional<C4SocketFactory>              _socketFactory;
        C4ConnectedClientStatusChangedCallback _onStatusChanged;
        C4ConnectedClientBlobProviderCallback  _blobProvider;
        void*                                  _callbackContext;
        alloc_slice                            _responseHeaders;
#ifdef COUCHBASE_ENTERPRISE
        mutable alloc_slice      _peerTLSCertificateData;
        mutable Retained<C4Cert> _peerTLSCertificate;
#endif
    };

}  // namespace litecore::client

fleece::Retained<C4ConnectedClient> C4ConnectedClient::newClient(C4Database*                        db,
                                                                 const C4ConnectedClientParameters& params) {
    return new litecore::client::C4ConnectedClientImpl(db, params);
}
