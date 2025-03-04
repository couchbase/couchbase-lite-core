//
// c4PeerDiscovery.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4PeerDiscovery.hh"
#include "c4Log.h"
#include "c4Socket.hh"
#include "Error.hh"
#include "Logging.hh"
#include "ObserverList.hh"
#include <algorithm>
#include <semaphore>

using namespace std;
using namespace fleece;
using namespace litecore;

namespace litecore::p2p {
    extern LogDomain P2PLog;
    LogDomain        P2PLog("P2P");
}  // namespace litecore::p2p

C4LogDomain const kC4P2PLog = (C4LogDomain)&litecore::p2p::P2PLog;


static void notify(C4Peer* peer, void (C4PeerDiscovery::Observer::*method)(C4Peer*));


#pragma mark - PEER:

string C4Peer::displayName() const {
    unique_lock lock(_mutex);
    return _displayName;
}

void C4Peer::setDisplayName(std::string_view name) {
    unique_lock lock(_mutex);
    _displayName = string(name);
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
    resolvedURL("", {NetworkDomain, kC4NetErrHostUnreachable});  // cancel resolve attempt
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
    _resolveURLCallback         = {};
    lock.unlock();

    if ( callback ) callback(std::move(url), provider->getSocketFactory(), error);
}

#pragma mark - DISCOVERY:


static mutex                                    sDiscoveryMutex;
static vector<C4PeerDiscoveryProvider*>         sProviders;
static ObserverList<C4PeerDiscovery::Observer*> sObservers;
static unordered_map<string, Retained<C4Peer>>  sPeers;

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

void C4PeerDiscovery::shutdown() {
    auto provs = providers();
    LogToAt(litecore::p2p::P2PLog, Info, "Shutting down peer discovery...");
    counting_semaphore<> sem(0);
    for ( C4PeerDiscoveryProvider* provider : provs ) {
        provider->shutdown([&]() { sem.release(); });
    }
    // Now wait for each to finish:
    for ( size_t i = 0; i < provs.size(); ++i ) sem.acquire();

    unique_lock lock(sDiscoveryMutex);
    Assert(sPeers.empty());
    Assert(sObservers.size() == 0);
    for ( C4PeerDiscoveryProvider* provider : provs ) delete provider;
    sProviders.clear();
    LogToAt(litecore::p2p::P2PLog, Info, "...peer discovery is shut down.");
}

#pragma mark - PROVIDER:

void C4PeerDiscoveryProvider::registerProvider() {
    unique_lock lock(sDiscoveryMutex);
    sProviders.push_back(this);
}

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
        if ( !handled ) handled = obs->incomingConnection(peer, socket);
    });
    if ( !handled ) {
        LogToAt(p2p::P2PLog, Warning, "No observer handled incoming connection from %s",
                (peer ? peer->id.c_str() : "??"));
    }
    return handled;
}
