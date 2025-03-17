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
    extern LogDomain DiscoveryLog, P2PLog;
    LogDomain        DiscoveryLog("Discovery");
    LogDomain        P2PLog("P2P");
}  // namespace litecore::p2p

C4LogDomain const kC4DiscoveryLog = (C4LogDomain)&litecore::p2p::DiscoveryLog;


#pragma mark - PEER:

string C4Peer::displayName() const {
    unique_lock lock(_mutex);
    return _displayName;
}

void C4Peer::setDisplayName(string_view name) {
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
    provider->discovery().notifyMetadataChanged(this);
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

    if ( callback ) {
        optional<C4SocketFactory> factory = provider->getSocketFactory();
        callback(std::move(url), (factory ? &*factory : nullptr), error);
    }
}

#pragma mark - DISCOVERY:


static mutex                                                   sFactoriesMutex;
static unordered_map<string, C4PeerDiscovery::ProviderFactory> sProviderFactories;

void C4PeerDiscovery::registerProvider(string_view providerName, ProviderFactory factory) {
    unique_lock lock(sFactoriesMutex);
    sProviderFactories.emplace(providerName, factory);
}

std::vector<std::string> C4PeerDiscovery::registeredProviders() {
    unique_lock    lock(sFactoriesMutex);
    vector<string> names;
    for ( auto& [name, factory] : sProviderFactories ) names.push_back(name);
    return names;
}

C4PeerDiscovery::C4PeerDiscovery(string_view serviceID) {
    unique_lock lock(sFactoriesMutex);
    Assert(!sProviderFactories.empty(), "No C4PeerDiscoveryProviders have been registered");
    for ( auto& [name, factory] : sProviderFactories ) _providers.emplace_back(factory(*this, serviceID));
}

C4PeerDiscovery::C4PeerDiscovery(std::string_view serviceID, std::span<std::string_view const> providers) {
    Assert(!providers.empty());
    unique_lock lock(sFactoriesMutex);
    for ( auto& name : providers ) {
        if ( auto i = sProviderFactories.find(string(name)); i != sProviderFactories.end() )
            _providers.emplace_back(i->second(*this, serviceID));
        else
            error::_throw(error::Unimplemented, "'%.*s' is not a registered peer discovery service",
                          FMTSLICE(slice(name)));
    }
}

C4PeerDiscovery::~C4PeerDiscovery() {
    LogToAt(litecore::p2p::DiscoveryLog, Info, "Shutting down C4PeerDiscovery...");
    counting_semaphore<> sem(0);
    for ( auto& provider : _providers ) provider->shutdown([&]() { sem.release(); });
    // Now wait for each to finish:
    for ( size_t i = 0; i < _providers.size(); ++i ) sem.acquire();

    unique_lock lock(_mutex);
    Assert(_peers.empty());
    LogToAt(litecore::p2p::DiscoveryLog, Info, "...C4PeerDiscovery shut down.");
}

void C4PeerDiscovery::startBrowsing() {
    for ( auto& provider : _providers ) provider->startBrowsing();
}

void C4PeerDiscovery::stopBrowsing() {
    for ( auto& provider : _providers ) provider->stopBrowsing();
}

void C4PeerDiscovery::startPublishing(string_view displayName, uint16_t port, C4Peer::Metadata const& md) {
    for ( auto& provider : _providers ) provider->startPublishing(displayName, port, md);
}

void C4PeerDiscovery::stopPublishing() {
    for ( auto& provider : _providers ) provider->stopPublishing();
}

void C4PeerDiscovery::updateMetadata(C4Peer::Metadata const& metadata) {
    for ( auto& provider : _providers ) provider->updateMetadata(metadata);
}

unordered_map<string, Retained<C4Peer>> C4PeerDiscovery::peers() {
    unique_lock lock(_mutex);
    return _peers;  // creates a copy
}

fleece::Retained<C4Peer> C4PeerDiscovery::peerWithID(string_view id) {
    unique_lock lock(_mutex);
    if ( auto i = _peers.find(string(id)); i != _peers.end() ) return i->second;
    return nullptr;
}

void C4PeerDiscovery::addObserver(Observer* obs) { _observers.add(obs); }

void C4PeerDiscovery::removeObserver(Observer* obs) { _observers.remove(obs); }

void C4PeerDiscovery::browseStateChanged(C4PeerDiscoveryProvider* provider, bool state, C4Error error) {
    unique_lock              lock(_mutex);
    vector<Retained<C4Peer>> removedPeers;
    if ( state == false ) {
        for ( auto i = _peers.begin(); i != _peers.end(); ) {
            if ( i->second->provider == provider ) {
                removedPeers.emplace_back(i->second);
                i = _peers.erase(i);
            } else {
                ++i;
            }
        }
    }
    lock.unlock();

    _observers.notify(&Observer::browsing, provider, state, error);

    for ( auto& peer : removedPeers ) {
        peer->removed();
        _observers.notify(&Observer::removedPeer, peer.get());
    }
}

void C4PeerDiscovery::publishStateChanged(C4PeerDiscoveryProvider* provider, bool state, C4Error error) {
    _observers.notify(&Observer::publishing, provider, state, error);
}

Retained<C4Peer> C4PeerDiscovery::addPeer(C4Peer* peer) {
    unique_lock lock(_mutex);
    auto [i, added] = _peers.insert({peer->id, peer});
    lock.unlock();

    if ( added ) {
        _observers.notify(&Observer::addedPeer, peer);
        return peer;
    } else {
        Assert(peer->provider == i->second->provider, "C4Peers of different providers have same ID '%s'",
               peer->id.c_str());
        return i->second;
    }
}

bool C4PeerDiscovery::removePeer(string_view id) {
    unique_lock      lock(_mutex);
    Retained<C4Peer> peer = nullptr;
    if ( auto i = _peers.find(string(id)); i != _peers.end() ) {
        peer = std::move(i->second);
        _peers.erase(i);
    }
    lock.unlock();

    if ( !peer ) return false;

    peer->removed();
    _observers.notify(&Observer::removedPeer, peer.get());
    return true;
}

bool C4PeerDiscovery::notifyIncomingConnection(C4Peer* peer, C4Socket* socket) {
    bool handled = false;
    _observers.iterate([&](auto obs) {
        // Only one Observer gets to handle it, so stop calling them after one returns true.
        if ( !handled ) handled = obs->incomingConnection(peer, socket);
    });
    if ( !handled ) {
        LogToAt(p2p::P2PLog, Warning, "No C4PeerDiscovery observer handled incoming connection from %s",
                (peer ? peer->id.c_str() : "??"));
    }
    return handled;
}

void C4PeerDiscovery::notifyMetadataChanged(C4Peer* peer) { _observers.notify(&Observer::peerMetadataChanged, peer); }
