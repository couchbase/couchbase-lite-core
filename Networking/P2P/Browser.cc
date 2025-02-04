//
// Created by Jens Alfke on 2/3/25.
//

#include "Browser.hh"
#include "Error.hh"
#include "Logging.hh"

namespace litecore::p2p {
    using namespace std;

    LogDomain P2PLog("P2P");


    const char* const Browser::kEventNames[] = {
        "BrowserOnline",
        "BrowserOffline",
        "BrowserStopped",
        "PeerAdded",
        "PeerRemoved",
        "PeerTxtChanged",
    };


    void Browser::notify(Event event, Peer* peer) {
        try {
            _observer(*this, event, peer);
        } catch (...) {}//FIXME
    }


    Browser::Browser(string_view serviceName, Observer obs)
    :Logging(P2PLog)
    ,_serviceName(serviceName)
    ,_observer(std::move(obs))
    {}


    Retained<Peer> Browser::peerNamed(string const& name) {
        std::unique_lock lock(_mutex);
        if (auto i = _peers.find(name); i != _peers.end())
            return i->second;
        return nullptr;
    }


    void Browser::addPeer(Retained<Peer> peer) {
        std::unique_lock lock(_mutex);
        bool added = _peers.emplace(peer->name(), peer).second;
        Assert(added, "Peer '%s' already registered", peer->name().c_str());

        lock.unlock();
        notify(PeerAdded, peer);
    }


    void Browser::removePeer(string const& name) {
        if (Retained<Peer> peer = peerNamed(name)) {
            std::unique_lock lock(_mutex);
            _peers.erase(name);

            lock.unlock();
            notify(PeerRemoved, peer);
        }
    }

}
