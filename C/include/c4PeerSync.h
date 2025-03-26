//
// c4PeerSync.h
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
#include "c4PeerSyncTypes.h"

#ifdef COUCHBASE_ENTERPRISE
C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** Creates a new C4PeerSync, but doesn't start it.
    @note You are responsible for freeing the returned reference with `c4peersync_free`. */
CBL_CORE_API C4PeerSync* C4NULLABLE c4peersync_new(const C4PeerSyncParameters*, C4Error* C4NULLABLE outError) C4API;

/** Returns this instance's peer UUID, as visible to other peers.
    (The UUID is derived from the C4Cert given in the parameters.)
    @note  This function is thread-safe. */
CBL_CORE_API C4PeerID c4peersync_getUUID(C4PeerSync*) C4API;

/** Starts a C4PeerSync, beginning peer discovery and replication.
    This call is asynchronous and returns immediately. When it succeeds or fails, the
    `C4PeerSync_StatusCallback` will be called.
    @note  This function is thread-safe. */
CBL_CORE_API void c4peersync_start(C4PeerSync*) C4API;

/** Stops a C4PeerSync. Stops all active replicators, stops the listener, and stops peer discovery
    and publishing.
    This call is asynchronous and returns immediately. When complete, the
    `C4PeerSync_StatusCallback` will be called.
    @note  This function is thread-safe. */
CBL_CORE_API void c4peersync_stop(C4PeerSync*) C4API;

C4API_END_DECLS
C4_ASSUME_NONNULL_END
#endif  // COUCHBASE_ENTERPRISE
