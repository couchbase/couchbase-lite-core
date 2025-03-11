//
// P2PSyncManager.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#    include "Actor.hh"
#    include "Base.hh"
#include "c4Database.hh"
#    include "c4Listener.hh"
#    include "c4PeerDiscovery.hh"
#    include "c4ReplicatorTypes.h"
#include "MetaPeer.hh"
#include <span>
#    include <variant>

#ifdef COUCHBASE_ENTERPRISE

namespace litecore {
    class DatabasePool;

    namespace REST {
        class SyncListener;
    }
}  // namespace litecore

C4_ASSUME_NONNULL_BEGIN

namespace litecore::p2p {

    struct CopiedReplicatorParameters : public C4ReplicatorParameters {
        explicit CopiedReplicatorParameters(C4ReplicatorParameters const&);
        CopiedReplicatorParameters(CopiedReplicatorParameters const& params)
        :CopiedReplicatorParameters((C4ReplicatorParameters const&)params) {}
    private:
        std::unique_ptr<C4ReplicationCollection[]> _collections;
        std::vector<alloc_slice>                   _slices;
    };

    /** High-level manager for peer-to-peer database sync. */
    class SyncManager
    : public actor::Actor
    , private C4PeerDiscovery::Observer {
    public:
        using DatabaseOrPool = std::variant<Retained<C4Database>, Retained<DatabasePool>>;

        /// Creates a SyncManager on a database.
        explicit SyncManager(DatabaseOrPool,
                             std::span<const std::string_view> providers,
                             string_view serviceID,
                             C4ReplicatorParameters const&);
        ~SyncManager();

        void stop() { enqueue(FUNCTION_TO_QUEUE(SyncManager::_stop)); }

    private:
        class ReplicateTask;

        // C4PeerDiscovery::Observer API:
        void addedPeer(C4Peer* peer) override;
        void removedPeer(C4Peer* peer) override;
        void peerMetadataChanged(C4Peer* peer) override;
        bool incomingConnection(C4Peer*, C4Socket*) override;

        void _stop();
        void _addedPeer(Retained<C4Peer>);
        void _removedPeer(Retained<C4Peer>);
        void _peerMetadataChanged(Retained<C4Peer>);
        void _incomingConnection(Retained<C4Peer>, C4Socket*);

        bool connectToPeer(MetaPeer*);

        Retained<DatabasePool>          _databasePool;
        std::string                     _databaseName;
        C4UUID                          _myUUID;
        C4PeerDiscovery                 _peerDiscovery;
        MetaPeers                       _metaPeers;
        Retained<REST::SyncListener> _syncListener;
        CopiedReplicatorParameters   _replicatorParams;
    };

}  // namespace litecore::p2p

C4_ASSUME_NONNULL_END

#endif  // COUCHBASE_ENTERPRISE
