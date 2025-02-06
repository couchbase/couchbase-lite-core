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
        "PeerAddressResolved",
        "PeerResolveFailed",
        "PeerTxtChanged",
    };


    void Browser::notify(Event event, Peer* peer) {
        try {
            _observer(*this, event, peer);
        } catch (...) {}//FIXME
    }


    Browser::Browser(string_view serviceType, string_view myName, Observer obs)
    :Logging(P2PLog)
    ,_serviceType(serviceType)
    ,_myName(myName)
    ,_observer(std::move(obs))
    {}


    uint16_t Browser::myPort() const {
        unique_lock lock(_mutex);
        return _myPort;
    }


    alloc_slice Browser::myTxtRecord() const {
        unique_lock lock(_mutex);
        return _myTxtRecord;
    }


    void Browser::setMyPort(uint16_t port) {
        unique_lock lock(_mutex);
        _myPort = port;
    }


    void Browser::setMyTxtRecord(alloc_slice txt) {
        unique_lock lock(_mutex);
        _myTxtRecord = txt;
    }


    Retained<Peer> Browser::peerNamed(string const& name) {
        unique_lock lock(_mutex);
        if (auto i = _peers.find(name); i != _peers.end())
            return i->second;
        return nullptr;
    }


    bool Browser::addPeer(Retained<Peer> peer) {
        unique_lock lock(_mutex);
        auto [i, added] = _peers.emplace(peer->name(), peer);
        if (added) {
            lock.unlock();
            notify(PeerAdded, peer);
        }
        return added;
    }


    void Browser::removePeer(string const& name) {
        if (Retained<Peer> peer = peerNamed(name)) {
            unique_lock lock(_mutex);
            _peers.erase(name);

            lock.unlock();
            notify(PeerRemoved, peer);
        }
    }


    optional<IPAddress> Peer::address() const {
        unique_lock lock(_mutex);
        return _address;
    }


    void Peer::setAddress(IPAddress const* addrp) {
        unique_lock lock(_mutex);
        if (addrp)
            _address = *addrp;
        else
            _address = nullopt;
    }

}
