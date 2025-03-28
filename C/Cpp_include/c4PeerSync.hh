//
// c4PeerSync.hh
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
#include "c4Base.hh"
#include "c4PeerSyncTypes.h"
#include "fleece/InstanceCounted.hh"

#ifdef COUCHBASE_ENTERPRISE
C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** A peer-to-peer sync manager that automatically discovers and connects with its counterparts
    with matching `peerGroupID`s, and replicates with them to sync a database. */
struct C4PeerSync : fleece::InstanceCounted {
  public:
    explicit C4PeerSync(C4PeerSyncParameters const&);

    /** @note  It is guaranteed that no more callbacks will be made after the destructor returns. */
    ~C4PeerSync() noexcept;

    /** Returns this instance's peer UUID, as visible to other peers.
        (The UUID is derived from the C4Cert given in the parameters.)
        @note  This function is thread-safe. */
    C4PeerID myID() const noexcept;

    /** Starts a C4PeerSync, beginning peer discovery and replication.
        This call is asynchronous and returns immediately. When it succeeds or fails, the
        `C4PeerSync_StatusCallback` will be called.
        @note  This function is thread-safe. */
    void start() noexcept;

    /** Stops a C4PeerSync. Stops all active replicators, stops the listener, and stops peer discovery
        and publishing.
        This call is asynchronous and returns immediately. When complete, the
        `C4PeerSync_StatusCallback` will be called.
        @note  This function is thread-safe. */
    void stop() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

C4_ASSUME_NONNULL_END
#endif  // COUCHBASE_ENTERPRISE
