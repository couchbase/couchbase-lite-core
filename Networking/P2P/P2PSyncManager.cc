//
// P2PSyncManager.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#ifdef COUCHBASE_ENTERPRISE

#    include "P2PSyncManager.hh"
#    include "DatabasePool.hh"
#    include "SecureRandomize.hh"
#    include "SyncListener.hh"

namespace litecore::p2p {
    using namespace std;

    extern LogDomain P2PLog;

    CopiedReplicatorParameters::CopiedReplicatorParameters(C4ReplicatorParameters const& params)
        : C4ReplicatorParameters(params) {
        auto makeAllocated = [this](auto& s) { s = _slices.emplace_back(s); };
        makeAllocated(optionsDictFleece);
        _collections = make_unique<C4ReplicationCollection[]>(collectionCount);
        std::copy(collections, collections + collectionCount, _collections.get());
        collections = _collections.get();
        for ( size_t i = 0; i < collectionCount; i++ ) {
            makeAllocated(collections[i].collection.name);
            makeAllocated(collections[i].collection.scope);
            makeAllocated(collections[i].optionsDictFleece);
        }
    }

    SyncManager::SyncManager(DatabaseOrPool db, string_view serviceID, C4ReplicatorParameters const& params)
        : Actor(P2PLog, "SyncManager"), _replicatorParams(params), _peerDiscovery(serviceID) {
        // Create or retain the database pool:
        if ( std::holds_alternative<Retained<C4Database>>(db) )
            _databasePool = make_retained<DatabasePool>(std::get<Retained<C4Database>>(db));
        else
            _databasePool = std::get<Retained<DatabasePool>>(db);

        // My ID as a peer is the database's public UUID:
        C4UUID uuid = BorrowedDatabase(_databasePool)->getPublicUUID();
        _myID       = slice(&uuid, sizeof(uuid)).hexString();

        // Start the TCP listener:
        C4ListenerConfig listenerConfig{
                .allowPush = true,
                .allowPull = true,
        };
        _tcpListener = make_retained<REST::SyncListener>(listenerConfig);
        _tcpListener->registerDatabase(_databasePool, "db");
        for ( size_t i = 0; i < _replicatorParams.collectionCount; i++ )
            _tcpListener->registerCollection("db", _replicatorParams.collections[i].collection);
        uint16_t port = _tcpListener->port();
        Assert(port != 0);

        // Start P2P stuff!
        _peerDiscovery.addObserver(this);
        _peerDiscovery.startBrowsing();
        _peerDiscovery.startPublishing(_myID, port, {});
    }

    void SyncManager::_stop() {
        _peerDiscovery.stopBrowsing();
        _peerDiscovery.stopPublishing();
        if ( _tcpListener ) {
            _tcpListener->stop();
            _tcpListener = nullptr;
        }
    }

    void SyncManager::_addedPeer(Retained<C4Peer> c4Peer) {
        //TODO
    }

    void SyncManager::_removedPeer(Retained<C4Peer> c4Peer) {
        //TODO
    }

    void SyncManager::_peerMetadataChanged(Retained<C4Peer> c4Peer) {
        //TODO
    }

}  // namespace litecore::p2p

#endif  // COUCHBASE_ENTERPRISE
