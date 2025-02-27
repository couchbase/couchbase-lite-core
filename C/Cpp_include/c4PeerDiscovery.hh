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

    /// True if the peer is online, false once it goes offline.
    /// @note  Once offline, an instance never comes back online; instead a new instance is created.
    bool online() const;

    /// Request to get the metadata of this peer and monitor it for changes, or to stop monitoring.
    /// When new metadata is available, C4PeerDiscovery observers' `peerMetadataChanged` methods will be called.
    void monitorMetadata(bool enable);

    /// Returns metadata (such as a TXT record entry) associated with a key, if any.
    fleece::alloc_slice getMetadata(std::string const& key) const;

    /// Returns all the metadata at once.
    Metadata getAllMetadata();

    /// Given an implementation-specific type name, returns a platform-specific object representing this peer.
    /// Default implementation simply returns nullptr. Subclasses should define what types and objects they support.
    virtual void* C4NULLABLE getPlatformPeer(fleece::slice typeName) const {return nullptr;}

    //---- Connections:

    using ResolveURLCallback = std::function<void(std::string, C4Error)>;

    /// Asynchronously finds the replication URL to connect to the peer.
    /// On completion, the callback will be invoked with either a URL string or a C4Error.
    /// To cancel resolution, call this again with a null callback.
    void resolveURL(ResolveURLCallback);

    using ConnectCallback = std::function<void(C4Socket* C4NULLABLE,C4Error)>;

    void connect(ConnectCallback);

    //---- Provider API:

    /// Updates the instance's displayName
    /// Should be called only by a subclass or a C4PeerDiscoveryProvider.
    void setDisplayName(std::string_view);

    /// Updates the instance's metadata.
    /// Should be called only by a subclass or a C4PeerDiscoveryProvider.
    void setMetadata(Metadata);

    /// Called by C4PeerDiscoveryProvider when it resolves this instance's URL or fails.
    void resolvedURL(std::string url, C4Error);
    bool connected(C4Socket* C4NULLABLE connection, C4Error);

    /// Called when an instance is about to be removed from the set of online peers.
    virtual void removed();

  protected:
    mutable std::mutex _mutex;

  private:
    std::string        _displayName;  ///< Arbitrary human-readable name registered by the peer
    Metadata           _metadata;
    ResolveURLCallback _resolveURLCallback;
    ConnectCallback     _connectCallback;
    bool               _online{true};
};

/** Singleton that provides the set of currently discovered C4Peers.
 *  @note  This class is thread-safe. */
class C4PeerDiscovery {
  public:
    /// Adds a provider implementation. Providers must be registered before calling startBrowsing.
    static void registerProvider(C4PeerDiscoveryProvider*);

    static std::vector<C4PeerDiscoveryProvider*> providers();

    /// Tells registered providers to start looking for peers.
    static void startBrowsing();

    /// Tells registered providers to stop looking for peers.
    static void stopBrowsing();

    static void startPublishing(std::string_view displayName, uint16_t port, C4Peer::Metadata const&);

    static void stopPublishing();

    static void updateMetadata(C4Peer::Metadata const&);

    /// Returns a copy of the current known set of peers.
    static std::unordered_map<std::string, fleece::Retained<C4Peer>> peers();

    /// Returns the peer (if any) with the given ID.
    static fleece::Retained<C4Peer> peerWithID(std::string_view id);

    /** API for receiving notifications of changes. */
    class Observer {
      public:
        virtual ~Observer();

        virtual void browsing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

        virtual void addedPeer(C4Peer*) {}

        virtual void removedPeer(C4Peer*) {}

        virtual void peerMetadataChanged(C4Peer*) {}

        virtual void publishing(C4PeerDiscoveryProvider*, bool active, C4Error) {}

        virtual bool incomingConnection(C4Peer*, C4Socket*) {return false;}
    };

    /// Registers an observer.
    static void addObserver(Observer*);
    /// Unregisters an observer.
    static void removeObserver(Observer*);

    C4PeerDiscovery() = delete;  // this is not an instantiable class
};

/** Abstract interface for a service that provides data for C4PeerDiscovery.
 *  Other code shouldn't call into this API; go through C4PeerDiscovery instead.
 *
 *  To implement a new protocol (DNS-SD, Bluetooth, ...): subclass this, implement the abstract
 *  methods, instantiate a singleton instance and register it with `C4PeerDiscovery`. Do not free it!
 *
 *  @note  This interface is thread-safe. Methods should be prepared to be called on arbitrary
 *         threads, and they may issue their own calls on arbitrary threads. */
class C4PeerDiscoveryProvider {
  public:
    explicit C4PeerDiscoveryProvider(std::string_view name_) : name(name_) {}

    virtual ~C4PeerDiscoveryProvider() = default;

    /// The provider's name, for logging/debugging purposes.
    std::string const name;

    /// Begin browsing for peers.
    /// Implementation must call \ref browseStateChanged when ready or on error. */
    virtual void startBrowsing() = 0;

    /// Stop browsing for peers. */
    /// Implementation must call \ref browseStateChanged when stopped. */
    virtual void stopBrowsing() = 0;

    /// Start/stop monitoring the metadata of a peer.
    /// Implementation must call the peer's \ref setMetadata whenever it receives metadata. */
    virtual void monitorMetadata(C4Peer*, bool start) = 0;

    /// Find the replication URL of the peer.
    /// Implementation must call \ref C4Peer::resolvedURL when done or on failure.
    virtual void resolveURL(C4Peer*) = 0;

    /// Cancel any in-progress resolveURL calls.
    virtual void cancelResolveURL(C4Peer*) = 0;

    virtual void connect(C4Peer*) = 0;
    virtual void cancelConnect(C4Peer*) = 0;

    virtual void publish(std::string_view displayName, uint16_t port, C4Peer::Metadata const&) = 0;
    virtual void unpublish()                                                                   = 0;
    virtual void updateMetadata(C4Peer::Metadata const&)                                       = 0;

  protected:
    /// Reports that browsing has started, stopped or failed.
    void browseStateChanged(bool state, C4Error = {});

    /// Reports that publishing has started, stopped or failed.
    void publishStateChanged(bool state, C4Error = {});

    /// Registers a newly discovered peer with to C4PeerDiscovery's set of peers, and returns it.
    /// If there is already a peer with this id, returns the existing one instead of registering the new one.
    fleece::Retained<C4Peer> addPeer(C4Peer*);

    /// Unregisters a peer that has gone offline.
    bool removePeer(C4Peer* peer) { return removePeer(peer->id); }

    /// Unregisters a peer that has gone offline.
    bool removePeer(std::string_view id);

    /// Notifies observers of an incoming connection from a peer.
    bool notifyIncomingConnection(C4Peer*, C4Socket*);
};

C4_ASSUME_NONNULL_END
