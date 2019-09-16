//
//  c4RemoteReplicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/16/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Replicator.hh"
#include "c4Socket+Internal.hh"
#include "Address.hh"

using namespace litecore::net;

namespace c4Internal {

    class C4RemoteReplicator : public C4Replicator {
    public:
        C4RemoteReplicator(C4Database* db,
                           const C4ReplicatorParameters &params,
                           const C4Address &serverAddress,
                           C4String remoteDatabaseName)
        :C4Replicator(db, params)
        ,_url(effectiveURL(serverAddress, remoteDatabaseName))
        { }


        void start(bool synchronous =false) override {
            if (_replicator)
                return;
            auto webSocket = CreateWebSocket(_url, socketOptions(), _database, _params.socketFactory);
            _replicator = new Replicator(_database, webSocket, *this, options());
            C4Replicator::start(synchronous);
        }


    private:

        // Returns URL string with the db name and "/_blipsync" appended to the Address's path
        static alloc_slice effectiveURL(C4Address address, slice remoteDatabaseName) {
            slice path = address.path;
            string newPath = string(path);
            if (!path.hasSuffix("/"_sl))
                newPath += "/";
            newPath += string(remoteDatabaseName) + "/_blipsync";
            address.path = slice(newPath);
            return Address::toURL(address);
        }


        // Options to pass to the C4Socket
        alloc_slice socketOptions() {
            string protocolString = string(Connection::kWSProtocolName) + kReplicatorProtocolName;
            Replicator::Options opts(kC4Disabled, kC4Disabled, _params.optionsDictFleece);
            opts.setProperty(slice(kC4SocketOptionWSProtocols),
                             protocolString.c_str());
            return opts.properties.data();
        }

    private:
        alloc_slice const   _url;
    };

}
