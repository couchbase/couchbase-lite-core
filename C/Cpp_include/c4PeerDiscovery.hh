//
// Created by Jens Alfke on 2/18/25.
//

#pragma once
#include "c4Base.h"
#include "c4Base.hh"
#include <array>
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


class C4PeerDiscoveryProvider;

/** A resolved address to connect to a C4Peer. */
struct C4PeerAddress {
    std::string address{};   ///< address in string form
    C4Timestamp expiration;  ///< time when this info becomes stale

    friend bool operator==(C4PeerAddress const&, C4PeerAddress const&) = default;
};

/** A discovered peer device.
 *  @note  This class is thread-safe.
 *  @note  This class is concrete, but may be subclassed by platform code if desired. */
class C4Peer : public fleece::RefCounted {
  public:
    C4Peer(C4PeerDiscoveryProvider* provider_, std::string id_, std::string displayName_)
        : provider(provider_), id(std::move(id_)), displayName(std::move(displayName_)) {}

    C4PeerDiscoveryProvider* const provider;  ///< Provider that manages this peer
    std::string const              id;        ///< Uniquely identifies this C4Peer (e.g. DNS-SD service name + domain)
    std::string const              displayName;  ///< Arbitrary human-readable name registered by the peer

    /// Request to discover or refresh the address(es) of this peer.
    /// When complete, the `addresses` property will be set and C4PeerDiscovery observers'
    /// `peerAddressesResolved` methods will be called.
    void resolveAddresses();

    /// All currently resolved and non-expired addresses.
    std::vector<C4PeerAddress> addresses();

    /// If address resolution failed, this property will be set.
    C4Error resolveError() const;

    using Metadata = std::unordered_map<std::string, fleece::alloc_slice>;

    /// Request to get the metadata of this peer and monitor it for changes, or to stop monitoring.
    /// When new metadata is available, C4PeerDiscovery observers' `peerMetadataChanged` methods will be called.
    void monitorMetadata(bool enable);

    /// Returns metadata (such as a TXT record entry) associated with a key, if any.
    fleece::alloc_slice getMetadata(std::string const& key) const;

    /// Returns all the metadata at once.
    Metadata getAllMetadata();

    //---- Provider API:

    /// Updates the instance's metadata.
    /// Should be called only by a subclass or a C4PeerDiscoveryProvider.
    void setMetadata(Metadata);

    /// Updates the instance's addresses.
    /// Should be called only by a subclass or a C4PeerDiscoveryProvider.
    void setAddresses(std::span<const C4PeerAddress>, C4Error = {});

  private:
    mutable std::mutex         _mutex;
    std::vector<C4PeerAddress> _addresses;
    Metadata                   _metadata;
    C4Error                    _error{};
};

/** Singleton that provides the set of currently discovered C4Peers.
 *  @note  This class is thread-safe. */
class C4PeerDiscovery {
  public:
    /// Adds a provider implementation. Providers must be registered before calling startBrowsing.
    static void registerProvider(C4PeerDiscoveryProvider*);

    /// Tells registered providers to start looking for peers.
    static void startBrowsing();

    /// Tells registered providers to stop looking for peers.
    static void stopBrowsing();

    /// Returns a copy of the current known set of peers.
    static std::unordered_map<std::string, fleece::Retained<C4Peer>> peers();

    /// Returns the peer (if any) with the given ID.
    static fleece::Retained<C4Peer> peerWithID(std::string_view id);

    /** API for receiving notifications of changes. */
    class Observer {
      public:
        virtual ~Observer();

        virtual void browsing(bool active, C4Error) {}

        virtual void addedPeer(C4Peer*) {}

        virtual void removedPeer(C4Peer*) {}

        virtual void peerMetadataChanged(C4Peer*) {}

        virtual void peerAddressesResolved(C4Peer*) {}
    };

    /// Registers an observer.
    static void addObserver(Observer*);
    /// Unregisters an observer.
    static void removeObserver(Observer*);

    C4PeerDiscovery() = delete;  // this is not an instantiable class
};

/** Interface for a service that provides data for C4PeerDiscovery.
 *  Other code shouldn't call it; go through C4PeerDiscovery instead.
 *  Platforms should subclass this to implement a specific protocol, and register an instance with
 *  C4PeerDiscovery at startup. Instances must not be deleted.
 *  @note  This interface is thread-safe. Subclasses should be prepared to be called on arbitrary
 *         threads, and they can issue calls on arbitrary threads. */
class C4PeerDiscoveryProvider {
  public:
    explicit C4PeerDiscoveryProvider(std::string_view name_) :name(name_) { }

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

    /// Provider callback that requests addresses be resolved for a peer.
    /// (This is a one-shot operation, not repeating like \ref monitorMetadata.)
    /// Implementation must call the peer's \ref setAddresses when done or on error. */
    virtual void resolveAddresses(C4Peer*) = 0;

  protected:
    /// Reports that browsing has started, stopped or failed.
    void browseStateChanged(bool state, C4Error = {});

    /// Registers a newly discovered peer with to C4PeerDiscovery's set of peers, and returns it.
    /// If there is already a peer with this id, returns the existing one instead of registering the new one.
    fleece::Retained<C4Peer> addPeer(C4Peer*);

    /// Removes a peer that is no longer online.
    bool removePeer(C4Peer* peer) { return removePeer(peer->id); }

    bool removePeer(std::string_view id);
};

C4_ASSUME_NONNULL_END
