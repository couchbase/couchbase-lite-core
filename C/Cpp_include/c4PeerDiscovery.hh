//
// c4PeerDiscovery.hh
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
#include "c4PeerSyncTypes.h"  // for C4PeerID
#include "c4Error.h"
#include "Observer.hh"
#include "fleece/InstanceCounted.hh"
#include "fleece/RefCounted.hh"
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#ifdef COUCHBASE_ENTERPRISE
C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************

struct C4Socket;
struct C4SocketFactory;

class C4Peer;
class C4PeerDiscoveryProvider;

/** The official logging channel of peer discovery. */
extern struct c4LogDomain* C4NONNULL const kC4DiscoveryLog;

/** The official logging channel of P2P sync. */
extern struct c4LogDomain* C4NONNULL const kC4P2PLog;

#    pragma mark - PEER DISCOVERY:

/** Manages peer discovery. To be used primarily by LiteCore's higher-level P2P functionality.
 *  For more details, read docs/P2P.md .
 *
 *  @note  This API is thread-safe. */
class C4PeerDiscovery {
  public:
    using ProviderDeleter = void (*)(C4PeerDiscoveryProvider*);
    using ProviderRef     = std::unique_ptr<C4PeerDiscoveryProvider, ProviderDeleter>;
    using ProviderFactory = ProviderRef (*)(C4PeerDiscovery& discovery, std::string_view peerGroupID);

    /// One-time registration of a provider class. The function will be called when constructing a C4PeerDiscovery.
    static void registerProvider(std::string_view providerName, ProviderFactory);

    static std::vector<std::string> registeredProviders();

    /// Constructor. Uses all registered providers.
    /// @param peerGroupID  An app-specific unique identifier. Will discover other devices that use this identifier.
    ///                   It must be 63 characters or less and may not contain `.`, `,` or `\\`.
    /// @param thisPeerID  This device's unique PeerID (generally a digest of an X.509 certificate.)
    explicit C4PeerDiscovery(std::string_view peerGroupID, C4PeerID const& thisPeerID);

    /// Constructor. Uses the named provider classes.
    /// @param peerGroupID  An app-specific unique identifier.
    /// @param thisPeerID  This device's unique PeerID (generally a digest of an X.509 certificate.)
    /// @param providerNames  A list of names of registered C4PeerDiscoveryProviders.
    explicit C4PeerDiscovery(std::string_view peerGroupID, C4PeerID const& thisPeerID,
                             std::span<const std::string_view> providerNames);

    /// The destructor shuts everything down in an orderly fashion, not returning until complete.
    ~C4PeerDiscovery();

    /// The peer group ID.
    std::string const& peerGroupID() const;

    /// This device's peer ID.
    C4PeerID const& peerID() const;

    /// The `C4PeerDiscoveryProvider`s in use.
    std::vector<ProviderRef> const& providers() const;

    /// Registers a default C4SocketFactory to be used when a Provider doesn't have a custom one.
    /// This factory is expected to handle normal IP-based WebSocket connections.
    /// @warning  Do not call this after there have been any calls to `startPublishing`.
    static void setDefaultSocketFactory(C4SocketFactory const&);

    /// Tells providers to start looking for peers.
    void startBrowsing();

    /// Tells providers to stop looking for peers.
    void stopBrowsing();

    using Metadata = std::unordered_map<std::string, fleece::alloc_slice>;

    /// Tells providers to advertise themselves so other devices can discover them.
    /// @param displayName  A user-visible name (optional)
    /// @param port  A port number, for protocols that need it (i.e. DNS-SD.)
    /// @param metadata  The peer metadata to advertise.
    void startPublishing(std::string_view displayName, uint16_t port, Metadata const& metadata);

    /// Stops publishing.
    void stopPublishing();

    /// Updates my published metadata, notifying interested peers.
    void updateMetadata(Metadata const&);

    /// Returns a copy of the current known set of peers.
    std::unordered_map<std::string, fleece::Ref<C4Peer>> peers();

    /// Returns the peer (if any) with the given ID.
    fleece::Retained<C4Peer> peerWithID(std::string_view id);

    /** API for receiving notifications from C4PeerDiscovery.
     *  @note Methods are called on arbitrary threads and may be called concurrently.
     *        They should return as soon as possible.
     *        It is OK for them to call back into `C4PeerDiscovery` or `C4Peer`.
     *  @warning Subclasses **must** call `removeFromObserverList()` in their destructor or earlier,
     *           to prevent receiving notifications after the observer is destructed! */
    class Observer : public litecore::Observer {
      public:
        /// Notification that a provider has started/stopped browsing for peers.
        virtual void browsing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

        /// Notification that one or more online peers have been discovered.
        virtual void addedPeers(std::span<fleece::Ref<C4Peer> const>) {}

        /// Notification that one or more peers have gone offline.
        virtual void removedPeers(std::span<fleece::Ref<C4Peer> const>) {}

        /// Notification that a peer's metadata has changed.
        virtual void peerMetadataChanged(C4Peer*) {}

        /// Notification that a provider has made this app discoverable by peers, or stopped.
        virtual void publishing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

        /// Notification of an incoming socket connection from a peer. (Only occurs while publishing is enabled.)
        /// @returns  True to accept the connection, false to reject it.
        virtual bool incomingConnection(C4Peer*, C4Socket*) { return false; }
    };

    /// Registers an observer.
    void addObserver(Observer*);

    /// Unregisters an observer.
    /// After this method returns, no more calls will be made to the observer and it can safely be destructed.
    void removeObserver(Observer*);

    /// Shuts down providers in an orderly fashion, not returning until complete.
    /// @note  It's not required to call this yourself; the destructor will call it.
    void shutdown();

    // Version number of c4PeerDiscovery.hh API. Incremented on incompatible changes.
    static constexpr int kAPIVersion = 10;

  protected:
    //---- Internal API for C4PeerDiscoveryProvider & C4Peer to call
    friend class C4Peer;
    friend class C4PeerDiscoveryProvider;

    void browseStateChanged(C4PeerDiscoveryProvider*, bool state, C4Error = {});

    void publishStateChanged(C4PeerDiscoveryProvider*, bool state, C4Error = {});

    fleece::Ref<C4Peer> addPeer(C4Peer*, bool moreComing);

    bool removePeer(std::string_view id, bool moreComing);

    void noMorePeersComing();

    bool notifyIncomingConnection(C4Peer*, C4Socket*);

    void notifyMetadataChanged(C4Peer*);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

#    pragma mark - PEER:

/** Represents a discovered peer device running the same peerGroupID.
 *  @note  This class is thread-safe.
 *  @note  This class is concrete, but may be subclassed by platform code if desired. */
class C4Peer
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4Peer> {
  public:
    using Metadata = C4PeerDiscovery::Metadata;

    C4Peer(C4PeerDiscoveryProvider* provider_, std::string id_) : provider(provider_), id(std::move(id_)) {}

    C4Peer(C4PeerDiscoveryProvider* provider_, std::string id_, Metadata md)
        : provider(provider_), id(std::move(id_)), _metadata(std::move(md)) {}

    /// The provider that manages this peer
    C4PeerDiscoveryProvider* const provider;

    /// Opaque string that uniquely identifies this `C4Peer` _instance_ across all providers.
    /// Examples are a DNS-SD service name + domain, or a Bluetooth LE peripheral UUID.
    std::string const id;

    /// True if the peer is in the set of active peers (C4PeerDiscovery::peers);
    /// false once it goes offline and is removed from the set.
    /// @note  Once offline, an instance never comes back online; instead a new instance is created.
    bool online() const { return _online; }

    /// True if it's OK to connect to the peer.
    /// Bluetooth peers return false if their signal strength is below a threshold.
    /// @note This property can sometimes change rapidly, so it does not trigger notifications.
    bool connectable() const { return _connectable; }

    //---- Metadata:

    /// Requests to start or stop monitoring (subscribing to) the metadata of this peer.
    /// When metadata changes, C4PeerDiscovery observers' \ref peerMetadataChanged methods will be called.
    void monitorMetadata(bool enable);

    /// Returns the metadata item (such as a TXT record entry) associated with a key, if any.
    fleece::alloc_slice getMetadata(std::string const& key) const;

    /// Returns all the metadata at once.
    Metadata getAllMetadata();

    //---- URLs:

    using ResolveURLCallback = std::function<void(std::string, const C4SocketFactory* C4NULLABLE, C4Error)>;

    /// Asynchronously finds the replication URL to connect to the peer.
    /// To cancel resolution, call this again with a null callback.
    /// On success, the callback will be invoked with a URL string and a socket factory to use for connections.
    /// On failure, the callback is invoked with an empty string and an error.
    /// @note  If the URL is already known, the callback will be invoked synchronously during this method.
    void resolveURL(ResolveURLCallback);

    //---- Record-keeping, for use by SyncManager:

    std::atomic<C4Timestamp> lastConnectionAttempt{};  ///< Last time a connection was attempted
    std::atomic<C4Error>     lastConnectionError{};    ///< Error (if any) of last connection

    //---- Methods below are for subclasses and C4PeerDiscoveryProviders only:

    /// Sets the `connectable` property. (No notifications are posted.)
    void setConnectable(bool c) { _connectable = c; }

    /// Sets the instance's metadata. If it's changed, posts a `peerMetadataChanged` notification.
    void setMetadata(Metadata);

    /// Invokes the current `ResolveURLCallback`, when a URL is resolved or on failure.
    void resolvedURL(std::string url, C4Error);

    /// Called when an instance is about to be removed from the set of online peers. Clears `online` & `metadata`.
    virtual void removed();

  private:
    mutable std::mutex _mutex;               // Must be locked while accessing state below (except atomics)
    Metadata           _metadata;            // Current known metadata
    ResolveURLCallback _resolveURLCallback;  // Holds callback during a resolveURL operation
    std::atomic<bool>  _online      = true;  // Set to false when peer is removed
    std::atomic<bool>  _connectable = true;  // Set by providers by calling setConnectable
};

#    pragma mark - PEER DISCOVERY PROVIDER:

/** Abstract interface for a service that provides data for C4PeerDiscovery.
 *  **Other code shouldn't call into this API**; go through C4PeerDiscovery instead.
 *
 *  To implement a new protocol (DNS-SD, Bluetooth, ...): subclass this and implement the abstract
 *  methods. At runtime, call \ref C4PeerDiscovery::registerProvider with a function that instantiates your class.
 *
 *  All the abstract methods are asynchronous (except for \ref getSocketFactory) and shouldn't block.
 *  The docs for each method specify what should be called when the operation is complete or fails.
 *
 *  For more details, read docs/P2P.md .
 *
 *  @note  This interface is thread-safe. Methods should be prepared to be called on arbitrary
 *         threads, and they may issue their own calls on arbitrary threads. */
class C4PeerDiscoveryProvider : public fleece::InstanceCounted {
  public:
    static constexpr std::string_view kDNS_SD      = "DNS-SD";       ///< Standard provider name
    static constexpr std::string_view kBluetoothLE = "BluetoothLE";  ///< Standard provider name

    explicit C4PeerDiscoveryProvider(C4PeerDiscovery& discovery, std::string_view providerName,
                                     std::string_view peerGroupID_)
        : name(providerName), peerGroupID(peerGroupID_), _discovery(discovery) {}

    /// The C4PeerDiscovery instance that owns this provider.
    C4PeerDiscovery& discovery() const { return _discovery; };

    /// The provider's name (e.g. "BluetoothLE") for identification/logging/debugging purposes.
    std::string const name;

    std::string const peerGroupID;

    bool isBrowsing() const { return _browsing; }

    bool isPublishing() const { return _publishing; }

    //-------- Abstract API for subclasses to implement:

    /// Begins browsing for peers.
    /// Implementation must call \ref browseStateChanged when ready or on error.
    /// While browsing, the implementation must call \ref addPeer and \ref removePeer as necessary.
    virtual void startBrowsing() = 0;

    /// Stops browsing for peers.
    /// Implementation must call \ref browseStateChanged when stopped.
    /// It doesn't need to call \ref removePeer; that will be done for it.
    virtual void stopBrowsing() = 0;

    /// Starts/stops monitoring the metadata of a peer.
    /// Implementation must call \ref C4Peer::setMetadata whenever it receives metadata.
    virtual void monitorMetadata(C4Peer*, bool start) = 0;

    /// Finds the replication URL of the peer.
    /// Implementation must call \ref C4Peer::resolvedURL when done or on failure.
    virtual void resolveURL(C4Peer*) = 0;

    /// Cancels any in-progress resolveURL calls.
    /// Default implementation does nothing.
    virtual void cancelResolveURL(C4Peer*) {}

    /// Returns the custom socket factory to use to connect to a peer URL, or nullopt if no special factory is needed.
    /// Default implementation returns the C4PeerDiscovery's defaultSocketFactory or `nullopt`.
    virtual std::optional<C4SocketFactory> getSocketFactory() const;

    /// Publishes/advertises a service so other devices can discover this one as a peer and connect to it.
    /// Implementation must call \ref publishStateChanged on success/failure.
    /// Implementation must call \ref notifyIncomingConnection when a peer connects.
    /// @param displayName  A user-visible name (optional)
    /// @param port  A port number, for protocols that need it (i.e. DNS-SD.)
    /// @param metadata  The peer metadata to advertise.
    virtual void startPublishing(std::string_view displayName, uint16_t port, C4Peer::Metadata const& metadata) = 0;

    /// Stops publishing.
    /// Implementation must call \ref publishStateChanged on completion.
    virtual void stopPublishing() = 0;

    /// Changes the published metadata. (No completion call needed.)
    virtual void updateMetadata(C4Peer::Metadata const&) = 0;

    /// Called when the owning \ref C4PeerDiscovery is being deleted.
    /// Implementation must stop browsing and publishing and any other ongoing tasks,
    /// then call the \ref onComplete callback, after which it will be deleted.
    virtual void shutdown(std::function<void()> onComplete) = 0;

  protected:
    /// Reports that browsing has started, stopped or failed.
    /// If `state` is false, this method will call \ref removePeer on all online peers.
    void browseStateChanged(bool state, C4Error error = {}) {
        _browsing = state;
        _discovery.browseStateChanged(this, state, error);
    }

    /// Reports that publishing has started, stopped or failed.
    void publishStateChanged(bool state, C4Error error = {}) {
        _publishing = state;
        _discovery.publishStateChanged(this, state, error);
    }

    /// Registers a newly discovered peer with to C4PeerDiscovery's set of peers, and returns it.
    /// If there is already a peer with this id, returns the existing one instead of registering the new one.
    /// (If you want to avoid creating a redundant peer, you can call \ref C4PeerDiscovery::peerWithID to check.)
    /// @param peer  The new C4Peer (or subclass) instance.
    /// @param moreComing  If you discover multiple peers at once, set this to `true` for all but
    ///                    the last one. This tells C4PeerDiscovery it can batch them all together
    ///                    into one `addedPeers` notification.
    fleece::Ref<C4Peer> addPeer(C4Peer* peer, bool moreComing = false) { return _discovery.addPeer(peer, moreComing); }

    /// Unregisters a peer that has gone offline.
    /// @param peer  The peer that went offline.
    /// @param moreComing  If multiple peers go offline at once, set this to `true` for all but
    ///                    the last one. This tells C4PeerDiscovery it can batch them all together
    ///                    into one `addedPeers` notification.
    bool removePeer(C4Peer* peer, bool moreComing = false) { return removePeer(peer->id, moreComing); }

    /// Unregisters any peer with this ID that has gone offline.
    /// @param id  The ID of the peer that went offline.
    /// @param moreComing  If multiple peers go offline at once, set this to `true` for all but
    ///                    the last one. This tells C4PeerDiscovery it can batch them all together
    ///                    into one `addedPeers` notification.
    bool removePeer(std::string_view id, bool moreComing) { return _discovery.removePeer(id, moreComing); }

    void noMorePeersComing() { _discovery.noMorePeersComing(); }

    /// Notifies observers about an incoming connection from a peer.
    /// @note  If the connection is not accepted, caller must close the C4Socket.
    /// @returns  true if the connection was accepted, false if not.
    bool notifyIncomingConnection(C4Peer* peer, C4Socket* s) { return _discovery.notifyIncomingConnection(peer, s); }

    /// The owning `C4PeerDiscovery` instance.
    C4PeerDiscovery& _discovery;

  private:
    std::atomic<bool> _browsing   = false;
    std::atomic<bool> _publishing = false;
};

#    pragma mark - STANDARD UUIDs AND IDS:

namespace litecore {
    class UUID;
}

namespace litecore::p2p {
    /// Namespace UUID used to construct peer UUIDs from certificates.
    /// This is combined with the certificate's DER data to produce a type-5 UUID that's the peer UUID.
    constexpr const char* kPeerCertUUIDNamespace = "A1F0F06F-F49A-4D9A-A08B-3B901D4ACD49";

    namespace btle {
        /// Namespace UUID used to construct BTLE service UUIDs.
        /// This is combined with the peerGroupID to produce a type-5 UUID that's the actual service UUID.
        constexpr const char* kPeerGroupUUIDNamespace = "E0C3793A-0739-42A2-A800-8BED236D8815";

        /// Constructs a BTLE service UUID from the `peerGroupID`.
        UUID ServiceUUIDFromPeerGroup(std::string_view peerGroup);

        /// Service characteristic whose value is the L2CAP port (PSM) the peer is listening on.
        /// (Apparently this is semi-standard? It appears in Apple's CoreBluetooth API headers.)
        constexpr const char* kPortCharacteristicID = "ABDD3056-28FA-441D-A470-55A75A52553A";

        /// Service characteristic whose value is the peer's Fleece-encoded metadata.
        constexpr const char* kMetadataCharacteristicID = "936D7669-E532-42BF-8B8D-97E3C1073F74";
    }  // namespace btle

    namespace dns_sd {
        /// DNS-SD service type. The peerGroupID is a subtype of this (concatenated with a comma).
        static constexpr const char* kBaseServiceType = "_couchbaseP2P._tcp";
    }  // namespace dns_sd
}  // namespace litecore::p2p

C4_ASSUME_NONNULL_END
#endif  // COUCHBASE_ENTERPRISE
