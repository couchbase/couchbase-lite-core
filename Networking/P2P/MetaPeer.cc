//
// Created by Jens Alfke on 3/10/25.
//

#include "MetaPeer.hh"
#include "c4PeerDiscovery.hh"
#include "Logging.hh"

namespace litecore::p2p {
    using namespace std;
    using namespace fleece;

    extern LogDomain P2PLog;


    template <typename T, typename U, size_t S>
    static bool addUnique(U&& item, smallVector<T,S>& vec) {
        if (auto j = std::ranges::find(vec, item); j == vec.end()) {
            vec.push_back(std::forward<U>(item));
            return true;
        }
        return false;
    }

    template <typename T, typename U, size_t S>
    static bool removeUnique(U const& item, smallVector<T,S>& vec) {
        if (auto j = std::ranges::find(vec, item); j != vec.end()) {
            vec.erase(j);
            return true;
        }
        return false;
    }



    bool clockwise(C4UUID const& a, C4UUID const& b) noexcept {
        int cmp = ::memcmp(&a, &b, sizeof(C4UUID));
        if ((a.bytes[0] & 0x80) != (b.bytes[0] & 0x80))
            cmp = -cmp;
        return cmp < 0;
    }


    MetaPeer::MetaPeer(C4UUID const& id, C4Peer* c4peer)
    :uuid(id)
    , _c4peers{c4peer}
    { }


    C4Peer* MetaPeer::bestC4Peer() const {
        C4Peer* best = nullptr;
        for (auto& c4peer : _c4peers) {
            if (c4peer->connectable()) {
                if (!best || c4peer->provider->name == "DNS-SD")    //TODO: Smarter comparison
                    best = c4peer;
            }
        }
        return best;
    }


    void MetaPeer::addTask(Task* task) {
        addUnique(task, _tasks);
    }


    void MetaPeer::removeTask(Task* task) {
        removeUnique(task, _tasks);
    }


    bool MetaPeer::addC4Peer(Retained<C4Peer> const& c4peer) {
        return addUnique(c4peer, _c4peers);
    }


    bool MetaPeer::removeC4Peer(Retained<C4Peer> const& c4peer) {
        return removeUnique(c4peer, _c4peers);
    }


    Retained<MetaPeer> MetaPeers::operator[](C4UUID const& id) const {
        if (auto i = _metaPeers.find(id); i != _metaPeers.end())
            return i->second;
        return nullptr;
    }


    Retained<MetaPeer> MetaPeers::metaPeerWithC4Peer(C4Peer* c4peer) const {
        if (auto i = _c4uuids.find(c4peer->id); i != _c4uuids.end())
            return (*this)[i->second];
        return nullptr;
    }


    Retained<MetaPeer> MetaPeers::addC4Peer(C4Peer* c4peer, C4UUID const& uuid) {
        Retained<MetaPeer> peer;
        if (auto i_id = _c4uuids.find(c4peer->id); i_id == _c4uuids.end()) {
            _c4uuids.emplace(c4peer->id, uuid);
            if (auto i_meta = _metaPeers.find(i_id->second); i_meta != _metaPeers.end()) {
                if (i_meta->second->addC4Peer(c4peer))
                    peer = i_meta->second;
            } else {
                peer = make_retained<MetaPeer>(uuid, c4peer);
                _metaPeers.emplace(uuid, peer);
            }
        } else if (uuid != i_id->second) {
            LogToAt(P2PLog, Warning, "C4Peer %s has changed its UUID!", c4peer->id.c_str());
        }
        return peer;
    }

    Retained<MetaPeer> MetaPeers::removeC4Peer(C4Peer* c4peer) {
        if (auto i_id = _c4uuids.find(c4peer->id); i_id != _c4uuids.end()) {
            if (auto i_meta = _metaPeers.find(i_id->second); i_meta != _metaPeers.end()) {
                Retained<MetaPeer> peer = i_meta->second;
                if (peer->removeC4Peer(c4peer)) {
                    // (don't remove `peer` from `_metaPeers`; keep them around)
                    return peer;
                }
            }
        }
        return nullptr;
    }

}
