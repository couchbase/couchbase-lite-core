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
    @note You are responsible for freeing the returned reference with `c4peersync_free`.
    @note  This function is thread-safe. */
CBL_CORE_API C4PeerSync* C4NULLABLE c4peersync_new(const C4PeerSyncParameters*, C4Error* C4NULLABLE outError) C4API;

/** Returns this instance's peer ID, as visible to other peers.
    (The ID is derived via \ref c4peerid_fromCert from the C4Cert given in the parameters.)
    @note  This function is thread-safe. */
CBL_CORE_API C4PeerID c4peersync_getMyID(C4PeerSync*) C4API;

/** Sets a C4PeerSync's progress-notification level. */
CBL_CORE_API void c4peersync_setProgressLevel(C4PeerSync*, C4ReplicatorProgressLevel) C4API;

/** Starts a C4PeerSync, beginning peer discovery and replication.
    This call is **asynchronous** and returns immediately. When it succeeds or fails, the
    `C4PeerSync_StatusCallback` will be called.
    @warning  You cannot re-start a C4PeerSync that's been stopped! Create a new one instead.
    @note  This function is thread-safe. */
CBL_CORE_API void c4peersync_start(C4PeerSync*) C4API;

/** Stops a C4PeerSync's active replicators, listener, peer discovery and publishing.
    This call is **asynchronous** and returns immediately. When complete, the
    `C4PeerSync_StatusCallback` will be called.
    @note  If \ref c4peersync_start has not been called, or \ref c4peersync_stop has already been
           called, this is a no-op and triggers no callbacks.
    @note  This function is thread-safe. */
CBL_CORE_API void c4peersync_stop(C4PeerSync*) C4API;

/** Returns the IDs of all online peers. `*outCount` will be set to the size of the array.
    @note  You are responsible for freeing the result via the regular `free()` function.
    @note  This function is thread-safe. */
CBL_CORE_API C4PeerID* C4NULLABLE c4peersync_getOnlinePeers(C4PeerSync*, size_t* outCount) C4API;

/** Gets information about a peer. Returns NULL if the peer ID is unknown.
    @note  You are responsible for freeing the result via \ref c4peerinfo_free.
    @note  This function is thread-safe. */
CBL_CORE_API C4PeerInfo* C4NULLABLE c4peersync_getPeerInfo(C4PeerSync*, C4PeerID) C4API;

/** Frees a C4PeerInfo reference.
    @note  This function is thread-safe, and it's safe to pass NULL to it. */
CBL_CORE_API void c4peerinfo_free(C4PeerInfo* C4NULLABLE) C4API;

/** Derives a C4PeerID from a `C4Cert`.
    @note  This function is thread-safe. */
CBL_CORE_API C4PeerID c4peerid_fromCert(C4Cert*) C4API;

/** Derives a C4PeerID from an X.509 certificate's DER (not PEM!) data.
    @note  This function is thread-safe. */
CBL_CORE_API C4PeerID c4peerid_fromCertData(C4Slice certData) C4API;

C4API_END_DECLS
C4_ASSUME_NONNULL_END
#endif  // COUCHBASE_ENTERPRISE
