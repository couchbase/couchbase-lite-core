//
// Created by Jens Alfke on 2/18/25.
//

#pragma once
#include "c4Base.hh"
#include "c4Error.h"
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
class C4PeerDiscoveryProvider;

extern struct c4LogDomain* C4NONNULL const kC4P2PLog;

/** A discovered peer device.
 *  @note  This class is thread-safe.
 *  @note  This class is concrete, but may be subclassed by platform code if desired. */
class C4Peer : public fleece::RefCounted {
  public:
    using Metadata = std::unordered_map<std::string, fleece::alloc_slice>;

    C4Peer(C4PeerDiscoveryProvider* provider_, std::string id_, std::string displayName_)
        : provider(provider_), id(std::move(id_)), _displayName(std::move(displayName_)) {}

    C4Peer(C4PeerDiscoveryProvider* provider_, std::string id_, std::string displayName_, Metadata const& md)
        : provider(provider_), id(std::move(id_)), _displayName(std::move(displayName_)), _metadata(md) {}

    C4PeerDiscoveryProvider* const provider;  ///< Provider that manages this peer
    std::string const              id;        ///< Uniquely identifies this C4Peer (e.g. DNS-SD service name + domain)

    /// Human-readable name, if any.
    std::string displayName() const;

    /// True if the peer is currently connectable. Bluetooth peers return false if their signal strength is too low.
    bool connectable() const { return _connectable; }

    /// True if the peer is in the set of active peers (C4PeerDiscovery::peers);
    /// false once it goes offline and is removed from the set.
    /// @note  Once offline, an instance never comes back online; instead a new instance is created.
    bool online() const { return _online; }

    /// Requests to start or stop monitoring (subscribing to) the metadata of this peer.
    /// When metadata changes, C4PeerDiscovery observers' `peerMetadataChanged` methods will be called.
    void monitorMetadata(bool enable);

    /// Returns the metadata (such as a TXT record entry) associated with a key, if any.
    fleece::alloc_slice getMetadata(std::string const& key) const;

    /// Returns all the metadata at once.
    Metadata getAllMetadata();

    //---- Connections:

    using ResolveURLCallback = std::function<void(std::string, C4Error)>;

    /// Asynchronously finds the replication URL to connect to the peer.
    /// On completion, the callback will be invoked with either a non-empty URL string or a C4Error.
    /// To cancel resolution, call this again with a null callback.
    void resolveURL(ResolveURLCallback);

    using ConnectCallback = std::function<void(void* C4NULLABLE, C4Error)>;

    /// Opens a connection to the peer.
    /// On completion, the callback will be invoked with either a non-null connection pointer` or a `C4Error`.
    /// The pointer type is implementation-defined.
    /// To cancel, call this again with a null callback.
    void connect(ConnectCallback);

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

  protected:
    mutable std::mutex _mutex;

  private:
    std::string        _displayName;  ///< Arbitrary human-readable name registered by the peer
    Metadata           _metadata;
    ResolveURLCallback _resolveURLCallback;
    ConnectCallback    _connectCallback;
    std::atomic<bool>  _online      = true;
    std::atomic<bool>  _connectable = true;
};

/** API for accessing peer discovery.
 *  @note  This API is thread-safe. */
class C4PeerDiscovery {
  public:
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
    static void startPublishing(std::string_view displayName, uint16_t port, C4Peer::Metadata const& metadata);

    /// Stops publishing.
    static void stopPublishing();

    /// Updates the published metadata.
    static void updateMetadata(C4Peer::Metadata const&);

    /// Returns a copy of the current known set of peers.
    static std::unordered_map<std::string, fleece::Retained<C4Peer>> peers();

    /// Returns the peer (if any) with the given ID.
    static fleece::Retained<C4Peer> peerWithID(std::string_view id);

    /** API for receiving notifications of changes.
        @note Methods are called on arbitrary threads and may be called concurrently.
              They should return as soon as possible.
              It is OK for them to call back into `C4PeerDiscovery` or `C4Peer`. */
    class Observer {
      public:
        virtual ~Observer() = default;

        /// Notification that a provider has started/stopped browsing.
        virtual void browsing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

        /// Notification that an online peer has been discovered.
        virtual void addedPeer(C4Peer*) {}

        /// Notification that a peer has gone offline.
        virtual void removedPeer(C4Peer*) {}

        /// Notification that a peer's metadata has changed.
        virtual void peerMetadataChanged(C4Peer*) {}

        /// Notification that a provider has started/stopped publishing itself.
        virtual void publishing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

        /// Notification of an incoming socket connection from a peer.
        /// @returns  True to accept the connection, false to reject it.
        virtual bool incomingConnection(C4Peer*, C4Socket*) { return false; }
    };

    /// Registers an observer.
    static void addObserver(Observer*);
    /// Unregisters an observer. After this method returns, it will no longer be called.
    static void removeObserver(Observer*);

    C4PeerDiscovery() = delete;  // this is not an instantiable class
};

/** Abstract interface for a service that provides data for C4PeerDiscovery.
 *  **Other code shouldn't call into this API**; go through C4PeerDiscovery instead.
 *
 *  To implement a new protocol (DNS-SD, Bluetooth, ...): subclass this, implement the abstract
 *  methods, instantiate a singleton instance and call its `registerProvider()` method. Do not free it!
 *
 *  @note  This interface is thread-safe. Methods should be prepared to be called on arbitrary
 *         threads, and they may issue their own calls on arbitrary threads. */
class C4PeerDiscoveryProvider {
  public:
    explicit C4PeerDiscoveryProvider(std::string_view name_) : name(name_) {}

    /// Registers a provider implementation with \ref C4PeerDiscovery.
    /// Providers must be registered before calling `C4PeerDiscovery::startBrowsing`.
    /// There is no facility to unregister a provider.
    void registerProvider();

    virtual ~C4PeerDiscoveryProvider() = default;

    /// The provider's name, for identification/logging/debugging purposes.
    std::string const name;

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

    /// Initiates a connection to a peer.
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

    /// Changes the published metadata.
    virtual void updateMetadata(C4Peer::Metadata const&) = 0;

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
