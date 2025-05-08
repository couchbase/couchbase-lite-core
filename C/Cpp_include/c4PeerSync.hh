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
#include "c4Replicator.hh"
#include "fleece/InstanceCounted.hh"
#include <cstring>  // for memcmp
#include <span>

#ifdef COUCHBASE_ENTERPRISE
C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** A peer-to-peer sync manager that automatically discovers and connects with its counterparts
    with matching `peerGroupID`s, and replicates with them to sync a database. */
struct C4PeerSync
    : fleece::InstanceCounted
    , C4Base {
  public:
    /** API to receive notifications from C4PeerSync. */
    class Delegate {
      public:
        virtual ~Delegate() = default;

        /// C4PeerSync has started or stopped, possibly with an error.
        virtual void peerSyncStatus(bool started, C4Error const&) {}

        /// A peer has come online or gone offline.
        virtual void peerDiscovery(C4PeerID const&, bool online) {}

        /// Authenticate a TLS connection to/from a peer, based on properties of its certificate.
        virtual bool authenticatePeer(C4PeerID const&, C4Cert*) = 0;

        /// A peer's direct connections to other peers have changed.
        virtual void peerNeighborsChanged(C4PeerID const&, size_t count) {}

        /// A replication with a peer has changed status.
        virtual void peerReplicationStatus(C4PeerID const&, C4ReplicatorStatus const&, bool incoming) {}

        /// A replication with a peer has transferred documents.
        /// @note  This will only be called if you configured Parameters::progressLevel accordingly.
        virtual void peerDocumentsEnded(C4PeerID const&, bool pushing, std::span<const C4DocumentEnded*>) {}

        /// A replication with a peer is transferring a blob.
        /// @note  This will only be called if you configured Parameters::progressLevel accordingly.
        virtual void peerBlobProgress(C4PeerID const&, bool pushing, C4BlobProgress const&) {}
    };

    /** Configuration of a C4PeerSync. */
    struct Parameters {
        std::string_view                  peerGroupID;        ///< App identifier for peer discovery
        std::span<const std::string_view> protocols;          ///< Which protocols to use (empty = all)
        C4Cert*                           tlsCert;            ///< My TLS certificate (server+client)
        C4KeyPair*                        tlsKeyPair;         ///< Certificate's key-pair
        C4Database*                       database;           ///< Database to sync
        std::span<C4PeerSyncCollection>   collections;        ///< Collections to sync
        slice                             optionsDictFleece;  ///< Replicator options
        C4ReplicatorProgressLevel         progressLevel;      ///< Level of progress notifications
        Delegate*                         delegate;           ///< Your object that receives notifications
    };

    explicit C4PeerSync(Parameters const&);
    explicit C4PeerSync(C4PeerSyncParameters const*);

    /** @note  It is guaranteed that the delegate will not be called after the destructor returns. */
    ~C4PeerSync() noexcept override;

    /// Returns this instance's peer UUID, as visible to other peers.
    /// (The UUID is derived from the C4Cert given in the parameters.)
    /// @note  This function is thread-safe.
    C4PeerID myPeerID() const noexcept;

    /// Starts a C4PeerSync, beginning peer discovery and replication.
    /// This call is asynchronous and returns immediately.
    /// When it succeeds or fails, the delegate's \ref peerSyncStatus method will be called.
    /// @note  This function is thread-safe.
    void start() noexcept;

    /// Stops all active replicators, stops the listener, and stops peer discovery and publishing.
    /// This call is asynchronous and returns immediately.
    /// When complete, the delegate's \ref peerSyncStatus method will be called.
    /// @note  This function is thread-safe.
    void stop() noexcept;

    /** Information about a peer, returned from \ref getPeerInfo. */
    struct PeerInfo {
        Retained<C4Cert>      certificate;         ///< Its identity; nullptr if unverified
        std::vector<C4PeerID> neighbors;           ///< Peers it's directly connected to
        C4ReplicatorStatus    replicatorStatus{};  ///< Status of my connection to it, if any
        bool                  online = false;      ///< True if it's currently online/visible
    };

    /// Returns a list of all peers currently online, including yourself.
    /// @note  This function is thread-safe.
    std::vector<C4PeerID> onlinePeers();

    /// Returns information about a peer.
    /// - If the peer is not directly connected, the `replicatorStatus.level` will be `kC4Stopped`.
    /// @note  This function is thread-safe.
    PeerInfo getPeerInfo(C4PeerID const&);

    // Version number of c4PeerSync.hh API. Incremented on incompatible changes.
    static constexpr int kAPIVersion = 3;

  private:
    class Impl;
    class CppImpl;
    class CImpl;
    std::unique_ptr<Impl> _impl;
};

inline bool operator==(C4PeerID const& a, C4PeerID const& b) { return memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0; }

C4_ASSUME_NONNULL_END
#endif  // COUCHBASE_ENTERPRISE
