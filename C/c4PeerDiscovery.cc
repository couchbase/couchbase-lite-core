//
// Created by Jens Alfke on 2/18/25.
//

#include "c4PeerDiscovery.hh"
#include "Logging.hh"
#include <algorithm>

using namespace std;
using namespace fleece;


#pragma mark - PEER:


void C4Peer::resolveAddresses() {
    C4PeerDiscoveryProvider::resolveAddresses(this);
}

vector<C4PeerAddress> C4Peer::addresses() const {
    unique_lock lock(_mutex);
    return _addresses;
}


void C4Peer::monitorMetadata(bool monitor) {
    C4PeerDiscoveryProvider::monitorMetadata(this, monitor);
}


alloc_slice C4Peer::getMetadata(string const& key) const {
    unique_lock lock(_mutex);
    if (auto i = _metadata.find(key); i != _metadata.end())
        return i->second;
    return nullslice;
}


C4Peer::Metadata C4Peer::getAllMetadata() {
    unique_lock lock(_mutex);
    return _metadata;
}


bool C4Peer::setMetadata(Metadata md) {
    unique_lock lock(_mutex);
    if (md == _metadata)
        return false;
    _metadata = std::move(md);
    return true;
}

bool C4Peer::setAddresses(std::span<const C4PeerAddress> addrs, C4Error error) {
    unique_lock lock(_mutex);
    if (error != _error) {
        _error = error;
        _addresses.clear();
        return true;
    } else if (addrs.size() != _addresses.size() || !std::equal(addrs.begin(), addrs.end(), _addresses.begin())) {
        _addresses.resize(addrs.size());
        std::ranges::copy(addrs, _addresses.begin());
        return true;
    } else {
        return false;
    }
}


#pragma mark - DISCOVERY:


static mutex sDiscoveryMutex;
static vector<C4PeerDiscovery::Observer*> sObservers;
static unordered_map<string, Retained<C4Peer>> sPeers;


void C4PeerDiscovery::startBrowsing() {
    C4PeerDiscoveryProvider::startBrowsing();
}

void C4PeerDiscovery::stopBrowsing() {
    C4PeerDiscoveryProvider::stopBrowsing();
}

unordered_map<string, Retained<C4Peer>> C4PeerDiscovery::peers() {
    unique_lock lock(sDiscoveryMutex);
    return sPeers;
}

fleece::Retained<C4Peer> C4PeerDiscovery::peerWithID(std::string_view id) {
    unique_lock lock(sDiscoveryMutex);
    if (auto i = sPeers.find(string(id)); i != sPeers.end())
        return i->second;
    return nullptr;
}

void C4PeerDiscovery::addObserver(Observer* obs) {
    unique_lock lock(sDiscoveryMutex);
    sObservers.push_back(obs);
}

void C4PeerDiscovery::removeObserver(Observer* obs) {
    unique_lock lock(sDiscoveryMutex);
    if (auto i = ranges::find(sObservers, obs); i != sObservers.end())
        sObservers.erase(i);
}


void C4PeerDiscovery::notify(C4Peer* peer, void(Observer::* method)(C4Peer*)) {
    unique_lock lock(sDiscoveryMutex);
    vector<Observer*> observers = sObservers;
    lock.unlock();

    for (auto obs : observers)
        (obs->*method)(peer);
}

void C4PeerDiscovery::notifyBrowsing(bool state, C4Error error) {
    unique_lock lock(sDiscoveryMutex);
    vector<Observer*> observers = sObservers;
    lock.unlock();

    for (auto obs : observers)
        obs->browsing(state, error);
}


C4PeerDiscovery::Observer::~Observer() = default;


#pragma mark - PROVIDER:


void C4PeerDiscoveryProvider::browsing(bool state, C4Error error) {
    C4PeerDiscovery::notifyBrowsing(state, error);
}


Retained<C4Peer> C4PeerDiscoveryProvider::addPeer(C4Peer* peer) {
    unique_lock lock(sDiscoveryMutex);
    auto [i, added] = sPeers.insert({peer->id, peer});
    lock.unlock();

    if (added) {
        C4PeerDiscovery::notify(peer, &C4PeerDiscovery::Observer::addedPeer);
        return peer;
    } else {
        return i->second;
    }
}

bool C4PeerDiscoveryProvider::removePeer(string_view id) {
    unique_lock lock(sDiscoveryMutex);
    C4Peer* peer = nullptr;
    if (auto i = sPeers.find(string(id)); i != sPeers.end()) {
        peer = i->second;
        sPeers.erase(i);
    }
    lock.unlock();

    if (!peer)
        return false;
    C4PeerDiscovery::notify(peer, &C4PeerDiscovery::Observer::removedPeer);
    return true;
}


void C4PeerDiscoveryProvider::setMetadata(C4Peer* peer, C4Peer::Metadata md) {
    if (peer->setMetadata(std::move(md)))
        C4PeerDiscovery::notify(peer, &C4PeerDiscovery::Observer::peerMetadataChanged);
}

void C4PeerDiscoveryProvider::setAddresses(C4Peer* peer, span<const C4PeerAddress> addrs, C4Error error) {
    if (peer->setAddresses(addrs, error))
        C4PeerDiscovery::notify(peer, &C4PeerDiscovery::Observer::peerMetadataChanged);
}
