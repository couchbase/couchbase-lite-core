//
// Created by Jens Alfke on 2/18/25.
//

#include "c4PeerDiscovery.hh"
#include "Error.hh"
#include "Logging.hh"
#include <algorithm>

using namespace std;
using namespace fleece;


static void notify(C4Peer* peer, void (C4PeerDiscovery::Observer::*method)(C4Peer*));


#pragma mark - PEER:

void C4Peer::resolveAddresses() { provider->resolveAddresses(this); }

vector<C4PeerAddress> C4Peer::addresses() {
    unique_lock lock(_mutex);
    C4Timestamp now = c4_now();
    for ( auto i = _addresses.begin(); i != _addresses.end(); ) {
        if ( i->expiration < now ) i = _addresses.erase(i);
        else
            ++i;
    }
    return _addresses;
}

C4Error C4Peer::resolveError() const {
    unique_lock lock(_mutex);
    return _error;
}

void C4Peer::monitorMetadata(bool monitor) { provider->monitorMetadata(this, monitor); }

alloc_slice C4Peer::getMetadata(string const& key) const {
    unique_lock lock(_mutex);
    if ( auto i = _metadata.find(key); i != _metadata.end() ) return i->second;
    return nullslice;
}

C4Peer::Metadata C4Peer::getAllMetadata() {
    unique_lock lock(_mutex);
    return _metadata;
}

void C4Peer::setMetadata(Metadata md) {
    {
        unique_lock lock(_mutex);
        if ( md == _metadata ) return;
        _metadata = std::move(md);
    }
    notify(this, &C4PeerDiscovery::Observer::peerMetadataChanged);
}

void C4Peer::setAddresses(std::span<const C4PeerAddress> addrs, C4Error error) {
    {
        unique_lock lock(_mutex);
        if ( error ) {
            _error = error;
            _addresses.clear();
        } else if ( !_error && addrs.size() == _addresses.size()
                    && std::equal(addrs.begin(), addrs.end(), _addresses.begin()) ) {
            return;  // unchanged
        } else {
            _error = kC4NoError;
            _addresses.resize(addrs.size());
            std::ranges::copy(addrs, _addresses.begin());
        }
    }
    notify(this, &C4PeerDiscovery::Observer::peerAddressesResolved);
}

#pragma mark - DISCOVERY:


static mutex                                   sDiscoveryMutex;
static vector<C4PeerDiscoveryProvider*>        sProviders;
static vector<C4PeerDiscovery::Observer*>      sObservers;
static unordered_map<string, Retained<C4Peer>> sPeers;

void C4PeerDiscovery::registerProvider(C4PeerDiscoveryProvider* provider) {
    unique_lock lock(sDiscoveryMutex);
    sProviders.push_back(provider);
}

std::vector<C4PeerDiscoveryProvider*> C4PeerDiscovery::providers() {
    unique_lock lock(sDiscoveryMutex);
    return sProviders;
}

void C4PeerDiscovery::startBrowsing() {
    for ( auto provider : providers() ) provider->startBrowsing();
}

void C4PeerDiscovery::stopBrowsing() {
    for ( auto provider : providers() ) provider->stopBrowsing();
}

void C4PeerDiscovery::startPublishing(std::string_view displayName, uint16_t port, C4Peer::Metadata const& md) {
    for ( auto provider : providers() ) provider->publish(displayName, port, md);
}

void C4PeerDiscovery::stopPublishing() {
    for ( auto provider : providers() ) provider->unpublish();
}

unordered_map<string, Retained<C4Peer>> C4PeerDiscovery::peers() {
    unique_lock lock(sDiscoveryMutex);
    return sPeers;
}

fleece::Retained<C4Peer> C4PeerDiscovery::peerWithID(std::string_view id) {
    unique_lock lock(sDiscoveryMutex);
    if ( auto i = sPeers.find(string(id)); i != sPeers.end() ) return i->second;
    return nullptr;
}

void C4PeerDiscovery::addObserver(Observer* obs) {
    unique_lock lock(sDiscoveryMutex);
    sObservers.push_back(obs);
}

void C4PeerDiscovery::removeObserver(Observer* obs) {
    unique_lock lock(sDiscoveryMutex);
    if ( auto i = ranges::find(sObservers, obs); i != sObservers.end() ) sObservers.erase(i);
}

static void notify(C4Peer* peer, void (C4PeerDiscovery::Observer::*method)(C4Peer*)) {
    unique_lock                        lock(sDiscoveryMutex);
    vector<C4PeerDiscovery::Observer*> observers = sObservers;
    lock.unlock();

    for ( auto obs : observers ) (obs->*method)(peer);
}

C4PeerDiscovery::Observer::~Observer() = default;


#pragma mark - PROVIDER:

void C4PeerDiscoveryProvider::browseStateChanged(bool state, C4Error error) {
    unique_lock              lock(sDiscoveryMutex);
    vector<Retained<C4Peer>> removedPeers;
    if ( state == false ) {
        for ( auto i = sPeers.begin(); i != sPeers.end(); ) {
            if ( i->second->provider == this ) {
                removedPeers.emplace_back(i->second);
                i = sPeers.erase(i);
            } else {
                ++i;
            }
        }
    }
    auto observers = sObservers;
    lock.unlock();

    for ( auto obs : observers ) obs->browsing(this, state, error);
    for ( auto& peer : removedPeers ) notify(peer, &C4PeerDiscovery::Observer::removedPeer);
}

void C4PeerDiscoveryProvider::publishStateChanged(bool state, C4Error error) {
    unique_lock lock(sDiscoveryMutex);
    auto        observers = sObservers;
    lock.unlock();

    for ( auto obs : observers ) obs->publishing(this, state, error);
}

Retained<C4Peer> C4PeerDiscoveryProvider::addPeer(C4Peer* peer) {
    unique_lock lock(sDiscoveryMutex);
    auto [i, added] = sPeers.insert({peer->id, peer});
    lock.unlock();

    if ( added ) {
        notify(peer, &C4PeerDiscovery::Observer::addedPeer);
        return peer;
    } else {
        Assert(peer->provider == i->second->provider, "C4Peers of different providers have same ID '%s'",
               peer->id.c_str());
        return i->second;
    }
}

bool C4PeerDiscoveryProvider::removePeer(string_view id) {
    unique_lock lock(sDiscoveryMutex);
    C4Peer*     peer = nullptr;
    if ( auto i = sPeers.find(string(id)); i != sPeers.end() ) {
        peer = i->second;
        sPeers.erase(i);
    }
    lock.unlock();

    if ( !peer ) return false;
    notify(peer, &C4PeerDiscovery::Observer::removedPeer);
    return true;
}
