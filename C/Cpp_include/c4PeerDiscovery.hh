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
#include "c4Error.h"
#include "fleece/InstanceCounted.hh"
#include <functional>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

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

extern struct c4LogDomain* C4NONNULL const kC4P2PLog;

/** API for accessing peer discovery. To be used primarily by LiteCore's higher-level P2P functionality.
 *  (This is more of a namespace than a class: all methods are static.)
 *  @note  This API is thread-safe. */
class C4PeerDiscovery {
  public:
    using Metadata = std::unordered_map<std::string, fleece::alloc_slice>;

    /// Returns all the registered provider instances.
    static std::vector<C4PeerDiscoveryProvider*> providers();

    /// Tells registered providers to start looking for peers.
    static void startBrowsing();

    /// Tells registered providers to stop looking for peers.
    static void stopBrowsing();

    /// Tells providers to advertise themselves so other devices can discover them.
    /// @param displayName  A user-visible name (optional)
    /// @param port  A port number, for protocols that need it (i.e. DNS-SD.)
    /// @param metadata  The peer metadata to advertise.
    static void startPublishing(std::string_view displayName, uint16_t port, Metadata const& metadata);

    /// Stops publishing.
    static void stopPublishing();

    /// Updates the published metadata.
    static void updateMetadata(Metadata const&);

    /// Returns a copy of the current known set of peers.
    static std::unordered_map<std::string, fleece::Retained<C4Peer>> peers();

    /// Returns the peer (if any) with the given ID.
    static fleece::Retained<C4Peer> peerWithID(std::string_view id);

    class Observer; // defined below

    /// Registers an observer.
    static void addObserver(Observer*);

    /// Unregisters an observer.
    /// After this method returns, no more calls will be made to it and it can safely be destructed.
    static void removeObserver(Observer*);


    //-------- For testing only:

    /// Resets the state of peer discovery, including stopping and unregistering all providers,
    /// and removing all observers.
    /// Does not return until this is completed.
    /// @note  This is not intended to be called in normal use. It's for use in tests.
    static void shutdown();

    C4PeerDiscovery() = delete;  // this is not an instantiable class
};


/** API for receiving notifications from C4PeerDiscovery.
    @note Methods are called on arbitrary threads and may be called concurrently.
          They should return as soon as possible.
          It is OK for them to call back into `C4PeerDiscovery` or `C4Peer`. */
class C4PeerDiscovery::Observer {
public:
    virtual ~Observer() = default;

    /// Notification that a provider has started/stopped browsing for peers.
    virtual void browsing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

    /// Notification that an online peer has been discovered.
    virtual void addedPeer(C4Peer*) {}

    /// Notification that a peer has gone offline.
    virtual void removedPeer(C4Peer*) {}

    /// Notification that a peer's metadata has changed.
    virtual void peerMetadataChanged(C4Peer*) {}

    /// Notification that a provider has made this app discoverable by peers, or stopped.
    virtual void publishing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

    /// Notification of an incoming socket connection from a peer.
    /// @returns  True to accept the connection, false to reject it.
    virtual bool incomingConnection(C4Peer*, C4Socket*) { return false; }
};


/** A discovered peer device.
 *  @note  This class is thread-safe.
 *  @note  This class is concrete, but may be subclassed by platform code if desired. */
class C4Peer
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4Peer> {
  public:
    using Metadata = C4PeerDiscovery::Metadata;

    C4Peer(C4PeerDiscoveryProvider* provider_, std::string id_, std::string displayName_)
        : provider(provider_), id(std::move(id_)), _displayName(std::move(displayName_)) {}

    C4Peer(C4PeerDiscoveryProvider* provider_, std::string id_, std::string displayName_, Metadata md)
        : provider(provider_), id(std::move(id_)), _displayName(std::move(displayName_)), _metadata(std::move(md)) {}

    /// The provider that manages this peer
    C4PeerDiscoveryProvider* const provider;

    /// Uniquely identifies this C4Peer across all providers.
    /// Examples are a DNS-SD service name + domain, or a Bluetooth LE peripheral UUID.
    std::string const id;

    /// Human-readable name, if any.
    std::string displayName() const;

    /// True if the peer is in the set of active peers (C4PeerDiscovery::peers);
    /// false once it goes offline and is removed from the set.
    /// @note  Once offline, an instance never comes back online; instead a new instance is created.
    bool online() const { return _online; }

    /// True if it's OK to connect to the peer.
    /// Bluetooth peers return false if their signal strength is below a threshold.
    /// @note This property can sometimes change rapidly, so it does not post notifications.
    bool connectable() const { return _connectable; }

    //---- Metadata:

    /// Requests to start or stop monitoring (subscribing to) the metadata of this peer.
    /// When metadata changes, C4PeerDiscovery observers' \ref peerMetadataChanged methods will be called.
    void monitorMetadata(bool enable);

    /// Returns the metadata item (such as a TXT record entry) associated with a key, if any.
    fleece::alloc_slice getMetadata(std::string const& key) const;

    /// Returns all the metadata at once.
    Metadata getAllMetadata();

    //---- URLs and Connections:

    using ResolveURLCallback = std::function<void(std::string, C4Error)>;

    /// Asynchronously finds the replication URL to connect to the peer.
    /// On completion, the callback will be invoked with either a non-empty URL string or a C4Error.
    /// To cancel resolution, call this again with a null callback.
    void resolveURL(ResolveURLCallback);

    using ConnectCallback = std::function<void(void* C4NULLABLE, C4Error)>;

    /// Opens a connection to the peer.
    /// On completion, the callback will be invoked with either a non-null connection pointer or a `C4Error`.
    /// The pointer type is implementation-defined.
    /// To cancel, call this again with a null callback.
    void connect(ConnectCallback);

    /// Cancels a connection attempt.
    void cancelConnect() { connect({}); }

    //---- Methods below are for subclasses and C4PeerDiscoveryProviders only:

    /// Updates the instance's displayName. (No notifications are posted.)
    void setDisplayName(std::string_view);

    /// Sets the `connectable` property. (No notifications are posted.)
    void setConnectable(bool c) { _connectable = c; }

    /// Sets the instance's metadata. If it's changed, posts a `peerMetadataChanged` notification.
    void setMetadata(Metadata);

    /// Invokes the current `ResolveURLCallback`, when a URL is resolved or on failure.
    void resolvedURL(std::string url, C4Error);

    /// Invokes the current `ConnectCallback`, on connection or failure.
    /// @returns True if the callback was called, false if it was canceled (so caller can close the socket.)
    bool connected(void* C4NULLABLE connection, C4Error);

    /// Called when an instance is about to be removed from the set of online peers. Clears `online` & `metadata`.
    virtual void removed();

  private:
    mutable std::mutex _mutex;               // Must be locked while accessing state below (except atomics)
    std::string        _displayName;         // Arbitrary human-readable name registered by the peer
    Metadata           _metadata;            // Current known metadata
    ResolveURLCallback _resolveURLCallback;  // Holds callback during a resolveURL operation
    ConnectCallback    _connectCallback;     // Holds callback during a connect operation
    std::atomic<bool>  _online      = true;  // Set to false when peer is removed
    std::atomic<bool>  _connectable = true;  // Set by providers by calling setConnectable
};


/** Abstract interface for a service that provides data for C4PeerDiscovery.
 *  **Other code shouldn't call into this API**; go through C4PeerDiscovery instead.
 *
 *  To implement a new protocol (DNS-SD, Bluetooth, ...): subclass this, implement the abstract
 *  methods, instantiate a singleton instance and call its `registerProvider()` method. Do not free it!
 *
 *  All the abstract methods are asynchronous (except for \ref getSocketFactory) and shouldn't block.
 *  The docs for each method specify what should be called when the operation is complete or fails.
 *
 *  @note  This interface is thread-safe. Methods should be prepared to be called on arbitrary
 *         threads, and they may issue their own calls on arbitrary threads. */
class C4PeerDiscoveryProvider : public fleece::InstanceCounted {
  public:
    explicit C4PeerDiscoveryProvider(std::string_view name_) : name(name_) {}

    /// Registers this provider implementation with \ref C4PeerDiscovery.
    /// Providers must be registered before calling `C4PeerDiscovery::startBrowsing`.
    /// There is no facility to unregister a provider.
    void registerProvider();

    /// The provider's name, for identification/logging/debugging purposes.
    std::string const name;

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
    /// Implementation must call the peer's \ref setMetadata whenever it receives metadata.
    virtual void monitorMetadata(C4Peer*, bool start) = 0;

    /// Finds the replication URL of the peer.
    /// Implementation must call \ref C4Peer::resolvedURL when done or on failure.
    virtual void resolveURL(C4Peer*) = 0;

    /// Cancels any in-progress resolveURL calls.
    virtual void cancelResolveURL(C4Peer*) = 0;

    /// Returns the custom socket factory to use to connect to a peer URL, or NULL if no special factory is needed.
    virtual C4SocketFactory const* C4NULLABLE getSocketFactory() const = 0;

    /// Called by \ref C4Peer::connect -- initiates a connection to a peer.
    /// Implementation must call \ref C4Peer::connected when done or on failure.
    virtual void connect(C4Peer*) = 0;

    /// Cancels a prior `connect` request.
    virtual void cancelConnect(C4Peer*) = 0;

    /// Publishes/advertises a service so other devices can discover this one as a peer.
    /// Implementation must call \ref publishStateChanged on success/failure.
    /// @param displayName  A user-visible name (optional)
    /// @param port  A port number, for protocols that need it (i.e. DNS-SD.)
    /// @param metadata  The peer metadata to advertise.
    virtual void publish(std::string_view displayName, uint16_t port, C4Peer::Metadata const& metadata) = 0;

    /// Stops publishing.
    /// Implementation must call \ref publishStateChanged on completion.
    virtual void unpublish() = 0;

    /// Changes the published metadata. (No completion call needed.)
    virtual void updateMetadata(C4Peer::Metadata const&) = 0;

    //-------- For testing only:

    /// Called by \ref C4PeerDiscovery::shutdown, generally as part of teardown of a test.
    /// Implementation must stop browsing and publishing and any other ongoing tasks,
    /// then call the `onComplete` callback, after which it will be deleted.
    virtual void shutdown(std::function<void()> onComplete) = 0;

    virtual ~C4PeerDiscoveryProvider() = default;

  protected:
    /// Reports that browsing has started, stopped or failed.
    /// If `state` is false, this method will call `removePeer` on all online peers.
    void browseStateChanged(bool state, C4Error = {});

    /// Reports that publishing has started, stopped or failed.
    void publishStateChanged(bool state, C4Error = {});

    /// Registers a newly discovered peer with to C4PeerDiscovery's set of peers, and returns it.
    /// If there is already a peer with this id, returns the existing one instead of registering the new one.
    /// (If you want to avoid creating a redundant peer, you can call \ref C4PeerDiscovery::peerWithID to check.)
    fleece::Retained<C4Peer> addPeer(C4Peer*);

    /// Unregisters a peer that has gone offline.
    bool removePeer(C4Peer* peer) { return removePeer(peer->id); }

    /// Unregisters any peer with this ID that has gone offline.
    bool removePeer(std::string_view id);

    /// Notifies observers of an incoming connection from a peer.
    bool notifyIncomingConnection(C4Peer*, C4Socket*);
};

C4_ASSUME_NONNULL_END
