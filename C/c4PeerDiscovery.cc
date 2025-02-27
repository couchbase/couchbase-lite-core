//
// Created by Jens Alfke on 2/18/25.
//

#include "c4PeerDiscovery.hh"
#include "c4Log.h"
#include "c4Socket.hh"
#include "Error.hh"
#include "Logging.hh"
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore::p2p {
    extern LogDomain P2PLog;
    LogDomain        P2PLog("P2P");
}

C4LogDomain const kC4P2PLog = (C4LogDomain)&litecore::p2p::P2PLog;


static void notify(C4Peer* peer, void (C4PeerDiscovery::Observer::*method)(C4Peer*));

template <class T>
class ObserverList {
  public:
    void add(T obs) {
        unique_lock lock(_mutex);
        _observers.emplace_back(std::move(obs));
    }

    void remove(T const& obs) {
        unique_lock lock(_mutex);
        if ( auto i = ranges::find(_observers, obs); i != _observers.end() ) {
            if ( i - _observers.begin() < _curIndex ) --_curIndex;  // Fix iterator if items shift underneath it
            _observers.erase(i);
        }
    }

    template <typename Callback>
    void iterate(Callback const& cb) const {
        unique_lock lock(_mutex);
        for ( _curIndex = ssize_t(_observers.size()) - 1; _curIndex >= 0; --_curIndex ) cb(_observers[_curIndex]);
    }

  private:
    mutable recursive_mutex _mutex;          // Recursive mutex allows re-entrant calls to add/remove during iteration
    vector<T>               _observers;      // The observer list
    mutable ssize_t         _curIndex = -1;  // Current iteration index, else -1
};

#pragma mark - PEER:

string C4Peer::displayName() const {
    unique_lock lock(_mutex);
    return _displayName;
}

void C4Peer::setDisplayName(std::string_view name) {
    unique_lock lock(_mutex);
    _displayName = string(name);
}

bool C4Peer::online() const {
    unique_lock lock(_mutex);
    return _online;
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

void C4Peer::removed() {
    unique_lock lock(_mutex);
    _online = false;
    _metadata.clear();
}

void C4Peer::resolveURL(ResolveURLCallback cb) {
    unique_lock lock(_mutex);
    bool        givenCallback = !!cb;
    Assert(!_resolveURLCallback || !givenCallback, "Multiple resolveURL requests to a C4Peer");
    _resolveURLCallback = std::move(cb);
    lock.unlock();
    if ( givenCallback ) provider->resolveURL(this);
    else
        provider->cancelResolveURL(this);
}

void C4Peer::resolvedURL(string url, C4Error error) {
    unique_lock        lock(_mutex);
    ResolveURLCallback callback = std::move(_resolveURLCallback);
    lock.unlock();

    if ( callback ) callback(std::move(url), error);
}

void C4Peer::connect(ConnectCallback cb) {
    unique_lock lock(_mutex);
    bool        givenCallback = !!cb;
    Assert(!_connectCallback || !givenCallback, "Multiple connect requests to a C4Peer");
    _connectCallback = std::move(cb);
    lock.unlock();
    if ( givenCallback ) provider->connect(this);
    else
        provider->cancelConnect(this);
}

bool C4Peer::connected(C4Socket* connection, C4Error error) {
    unique_lock        lock(_mutex);
    ConnectCallback callback = std::move(_connectCallback);
    lock.unlock();

    if (!callback) return false;
    callback(connection, error);
    return true;
}

#pragma mark - DISCOVERY:


static mutex                                    sDiscoveryMutex;
static vector<C4PeerDiscoveryProvider*>         sProviders;
static ObserverList<C4PeerDiscovery::Observer*> sObservers;
static unordered_map<string, Retained<C4Peer>>  sPeers;

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

void C4PeerDiscovery::updateMetadata(C4Peer::Metadata const& metadata) {
    for ( auto provider : providers() ) provider->updateMetadata(metadata);
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

void C4PeerDiscovery::addObserver(Observer* obs) { sObservers.add(obs); }

void C4PeerDiscovery::removeObserver(Observer* obs) { sObservers.remove(obs); }

static void notify(C4Peer* peer, void (C4PeerDiscovery::Observer::*method)(C4Peer*)) {
    sObservers.iterate([&](auto obs) { (obs->*method)(peer); });
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
    lock.unlock();

    sObservers.iterate([&](auto obs) { obs->browsing(this, state, error); });

    for ( auto& peer : removedPeers ) {
        peer->removed();
        notify(peer, &C4PeerDiscovery::Observer::removedPeer);
    }
}

void C4PeerDiscoveryProvider::publishStateChanged(bool state, C4Error error) {
    sObservers.iterate([&](auto obs) { obs->publishing(this, state, error); });
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
    unique_lock      lock(sDiscoveryMutex);
    Retained<C4Peer> peer = nullptr;
    if ( auto i = sPeers.find(string(id)); i != sPeers.end() ) {
        peer = std::move(i->second);
        sPeers.erase(i);
    }
    lock.unlock();

    if ( !peer ) return false;

    peer->removed();
    notify(peer, &C4PeerDiscovery::Observer::removedPeer);
    return true;
}

bool C4PeerDiscoveryProvider::notifyIncomingConnection(C4Peer* peer, C4Socket* socket) {
    bool handled = false;
    sObservers.iterate([&](auto obs) {
        if (!handled)
            handled = obs->incomingConnection(peer, socket);
    });
    if (!handled) {
        LogToAt(litecore::p2p::P2PLog, Warning, "No observer handled incoming connection from %s",
                (peer ? peer->id.c_str() : "??"));
    }
    return handled;
}
