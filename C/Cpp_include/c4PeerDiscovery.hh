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


/** A resolved address to connect to a C4Peer. */
struct C4PeerAddress {
    enum Type {
        IPv4, IPv6, BT
    };
    static constexpr size_t kMaxAddressSize = 32;   // ??? Is this enough for BT?
    using Data = std::array<std::byte,kMaxAddressSize>;

    Type        type;       ///< type of address; identifies structure of data
    C4Timestamp expiration; ///< time when this info becomes stale
    Data        data {};    ///< raw address data

    friend bool operator==(C4PeerAddress const&, C4PeerAddress const&) = default;
};


/** A discovered peer device. */
class C4Peer : public fleece::RefCounted {
public:
    C4Peer(std::string id_, std::string displayName_) :id(std::move(id_)), displayName(std::move(displayName_)) { }

    std::string const id;             ///< Uniquely identifies this C4Peer (e.g. DNS-SD service name + domain)
    std::string const displayName;    ///< Arbitrary human-readable name registered by the peer

    void resolveAddresses();

    /// All currently resolved addresses.
    std::vector<C4PeerAddress> addresses() const;

    using Metadata = std::unordered_map<std::string, fleece::alloc_slice>;

    void monitorMetadata(bool);

    /// Returns metadata (such as a TXT record entry) associated with a key, if any.
    fleece::alloc_slice getMetadata(std::string const& key) const;

    /// Returns all the metadata at once.
    Metadata getAllMetadata();

private:
    friend class C4PeerDiscoveryProvider;
    bool setMetadata(Metadata);
    bool setAddresses(std::span<const C4PeerAddress>, C4Error);

    std::mutex mutable          _mutex;
    std::vector<C4PeerAddress>  _addresses;
    Metadata                    _metadata;
    C4Error                     _error {};
};


/** Singleton interface that provides the set of currently discovered C4Peers. */
class C4PeerDiscovery {
public:
    static void startBrowsing();
    static void stopBrowsing();

    /// Returns a copy of the current known set of peers.
    static std::unordered_map<std::string, fleece::Retained<C4Peer>> peers();

    static fleece::Retained<C4Peer> peerWithID(std::string_view id);

    /** API for receiving notifications of changes. */
    class Observer {
    public:
        virtual ~Observer();
        virtual void browsing(bool active, C4Error) { }
        virtual void addedPeer(C4Peer*) { }
        virtual void removedPeer(C4Peer*) { }
        virtual void peerMetadataChanged(C4Peer*) { }
        virtual void peerAddressesResolved(C4Peer*) { }
    };

    static void addObserver(Observer*);
    static void removeObserver(Observer*);

private:
    friend class C4PeerDiscoveryProvider;
    C4PeerDiscovery() = delete;
    static void notify(C4Peer*, void (Observer::*method)(C4Peer*));
    static void notifyBrowsing(bool state, C4Error);
};


/** Singleton interface that provides the data for C4PeerDiscovery.
 *  Platform code should set the callbacks to point to its own functions,
 *  and respond (asynchronously) by calling the appropriate interface functions. */
class C4PeerDiscoveryProvider {
public:
    /// Provider callback that begins browsing for peers. */
    static inline void (*startBrowsing)();

    /// Provider callback that stops browsing for peers. */
    static inline void (*stopBrowsing)();

    /// Provider callback that starts or stops monitoring the metadata of a peer.
    static inline void (*monitorMetadata)(C4Peer*, bool start);

    /// Provider callback that requests addresses be resolved for a peer.
    /// This is a one-shot operation. The provider should call `resolvedAddresses` when done.
    static inline void (*resolveAddresses)(C4Peer*);

    /// Reports that browsing has stopped or failed.
    static void browsing(bool state, C4Error = {});

    /// Registers a newly discovered peer with to C4PeerDiscovery's set of peers, and returns it.
    /// Or if there is already a peer with this id, returns the existing one instead.
    static fleece::Retained<C4Peer> addPeer(C4Peer*);

    /// Removes a peer that is no longer online.
    static bool removePeer(C4Peer* peer)        {return removePeer(peer->id);}
    static bool removePeer(std::string_view id);

    /// Updates a peer's metadata.
    static void setMetadata(C4Peer*, C4Peer::Metadata);

    /// Updates a peer's addresses.
    static void setAddresses(C4Peer*, std::span<const C4PeerAddress>, C4Error = {});

private:
    C4PeerDiscoveryProvider() = delete;
};

C4_ASSUME_NONNULL_END
