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
#include "RevTree.hh"
#include "TreeDocument.hh"

namespace litecore::client {

    using namespace litecore::websocket;
    using namespace litecore::actor;
    using namespace std;
    
    struct C4ConnectedClientImpl: public C4ConnectedClient, public ConnectedClient::Delegate {
        
    public:
        C4ConnectedClientImpl(const C4ConnectedClientParameters &params) {
            if (params.socketFactory) {
                // Keep a copy of the C4SocketFactory struct in case original is invalidated:
                _customSocketFactory = *params.socketFactory;
                _socketFactory = &_customSocketFactory;
            }
            
            auto webSocket = new repl::C4SocketImpl(effectiveURL(params.url),
                                                    Role::Client,
                                                    socketOptions(),
                                                    _socketFactory);
            _client = new ConnectedClient(webSocket, *this, params);
            _client->start();
        }
    
    protected:
#pragma mark - ConnectedClient Delegate
        
        virtual void clientGotHTTPResponse(ConnectedClient* C4NONNULL client,
                                           int status,
                                           const websocket::Headers &headers) override {
            // TODO: implement
        }
        virtual void clientGotTLSCertificate(ConnectedClient* C4NONNULL client,
                                             slice certData) override {
            // TODO: implement
        }
        virtual void clientStatusChanged(ConnectedClient* C4NONNULL client,
                                         ConnectedClient::ActivityLevel level) override {
            // TODO: implement
        }
        virtual void clientConnectionClosed(ConnectedClient* C4NONNULL client, const CloseStatus& status) override {
            // TODO: implement
        }
        
#pragma mark -
        Async<DocResponse> getDoc(slice docID,
                                    slice collectionID,
                                    slice unlessRevID,
                                    bool asFleece) override {
            return _client->getDoc(docID,
                                   collectionID,
                                   unlessRevID,
                                   asFleece).then([](auto a) -> DocResponse {
                return { a.docID, a.revID, a.body, a.deleted };
            });
        }
        
        Async<std::string> putDoc(slice docID,
                             slice collectionID,
                             slice parentRevisionID,
                             C4RevisionFlags flags,
                             slice fleeceData) override {
            bool deletion = (flags & kRevDeleted) != 0;
            revidBuffer generatedRev = TreeDocumentFactory::generateDocRevID(fleeceData,
                                                                             parentRevisionID,
                                                                             deletion);
            auto provider = Async<std::string>::makeProvider();
            _client->putDoc(docID,
                            collectionID,
                            revid(generatedRev).expanded(),
                            parentRevisionID,
                            flags,
                            fleeceData).then([=](Result<void> i) {
                if (i.ok()) {
                    auto revID = revid(generatedRev).expanded();
                    provider->setResult(revID.asString());
                } else
                    provider->setError(i.error());
            });
            return provider->asyncValue();
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
            if (_client)
                _client->terminate();
            
            _client = nullptr;
        }
        
        alloc_slice effectiveURL(slice);
        alloc_slice socketOptions();
        
        mutable std::mutex                  _mutex;
        Retained<ConnectedClient>           _client;
        const C4SocketFactory* C4NULLABLE   _socketFactory {nullptr};
        C4SocketFactory                     _customSocketFactory {};  // Storage for *_socketFactory if non-null
    };
}
