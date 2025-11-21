//
// c4Replicator+Pool.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


#pragma once
#include "c4Replicator.hh"

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    class DatabasePool;

    namespace websocket {
        class WebSocket;
    }
}  // namespace litecore

// C4Replicator factory functions that take a DatabasePool instead of a C4Database.

fleece::Ref<C4Replicator> NewReplicator(litecore::DatabasePool* dbPool, C4Address serverAddress,
                                        fleece::slice remoteDatabaseName, const C4ReplicatorParameters& params,
                                        fleece::slice logPrefix = {});
fleece::Ref<C4Replicator> NewLocalReplicator(litecore::DatabasePool* dbPool, litecore::DatabasePool* otherLocalDB,
                                             const C4ReplicatorParameters& params, fleece::slice logPrefix = {});
fleece::Ref<C4Replicator> NewIncomingReplicator(litecore::DatabasePool*         dbPool,
                                                litecore::websocket::WebSocket* openSocket,
                                                const C4ReplicatorParameters& params, fleece::slice logPrefix = {});


C4_ASSUME_NONNULL_END
