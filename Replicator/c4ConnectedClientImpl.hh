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

#include "c4Base.h"
#include "ConnectedClient.hh"
#include "C4ConnectedClient.hh"
#include "c4Socket+Internal.hh"

namespace litecore::client {

    using namespace litecore::websocket;
    using namespace litecore::actor;
    
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
            _client = new ConnectedClient(webSocket, *this, fleece::AllocedDict(params.options));
            _client->start();
        }
    
    protected:
#pragma mark - ConnectedClient Delegate
        
        virtual void clientGotHTTPResponse(ConnectedClient* C4NONNULL client,
                                           int status,
                                           const websocket::Headers &headers) {
            // TODO: implement
        }
        virtual void clientGotTLSCertificate(ConnectedClient* C4NONNULL client,
                                             slice certData) {
            // TODO: implement
        }
        virtual void clientStatusChanged(ConnectedClient* C4NONNULL client,
                                         ConnectedClient::ActivityLevel level) {
            // TODO: implement
        }
        virtual void clientConnectionClosed(ConnectedClient* C4NONNULL client, const CloseStatus& status)  {
            // TODO: implement
        }
        
#pragma mark -
        virtual Async<C4DocResponse> getDoc(C4Slice docID,
                                            C4Slice collectionID,
                                            C4Slice unlessRevID,
                                            bool asFleece) noexcept {
            return _client->getDoc(docID,
                                   collectionID,
                                   unlessRevID,
                                   asFleece).then([](DocResponse a) -> C4DocResponse {
                return C4DocResponse {
                    .docID = a.docID,
                    .body = a.body,
                    .revID = a.revID,
                    .deleted = a.deleted,
                };
            });
        }
                
    private:
        ~C4ConnectedClientImpl() {
            if (_client)
                _client->stop();
        }
        
        alloc_slice effectiveURL(slice);
        alloc_slice socketOptions();
        
        Retained<ConnectedClient>           _client;
        const C4SocketFactory* C4NULLABLE   _socketFactory {nullptr};
        C4SocketFactory                     _customSocketFactory {};  // Storage for *_socketFactory if non-null
    };

}
