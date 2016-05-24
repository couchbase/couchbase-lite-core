//
//  VersionVector.cc
//  CBForest
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "VersionVector.hh"
#include "varint.hh"
#include "error.hh"
#include <sstream>
#include <unordered_map>


namespace cbforest {

    version::version(slice string) {
        gen = string.readDecimal();                             // read generation
        if (gen == 0 || string.readByte() != '@')               // read '@'
            throw error(error::BadVersionVector);
        peer = string;                                          // read peer ID
    }


    versionVector::versionVector(slice string)
    :_string(string)
    {
        while (string.size > 0) {
            const void *comma = string.findByte(',') ?: string.end();
            _vers.push_back( version(string.upTo(comma)) );
            string = string.from(comma);
            if (string.size > 0)
                string.moveStart(1); // skip comma
        }
    }

    alloc_slice versionVector::asString() const {
        if (!_changed && _string.buf)
            return _string;

        size_t dataSize = 0;
        for (auto v = _vers.begin(); v != _vers.end(); ++v)
            dataSize += slice::sizeOfDecimal(v->gen) + v->peer.size + 2;
        if (dataSize > 0)
            --dataSize;

        alloc_slice data(dataSize);
        slice out = data;
        for (auto v = _vers.begin(); v != _vers.end(); ++v) {
            if (v != _vers.begin())
                out.writeByte(',');
            out.writeDecimal(v->gen);
            out.writeByte('@');
            out.writeFrom(v->peer);
        }
        return data;
    }
    
    versionVector::order versionVector::compareTo(const versionVector &other) const {
        int o = kSame;
        ssize_t countDiff = count() - other.count();
        if (countDiff < 0)
            o = kOlder;
        else if (countDiff > 0)
            o = kNewer;

        for (auto v = _vers.begin(); v != _vers.end(); ++v) {
            auto othergen = other[v->peer];
            if (v->gen < othergen)
                o |= kOlder;
            else if (v->gen > othergen)
                o |= kNewer;
            else if (o == kSame)
                break; // first revs are identical so vectors are equal
            if (o == kConflicting)
                break;
        }
        return (order)o;
    }

    std::vector<version>::iterator versionVector::findPeerIter(peerID peer) {
        auto v = _vers.begin();
        for (; v != _vers.end(); ++v) {
            if (v->peer == peer)
                break;
        }
        return v;
    }

    generation versionVector::genOfPeer(peerID peer) const {
        auto v = const_cast<versionVector*>(this)->findPeerIter(peer);
        return (v != _vers.end()) ? v->gen : 0;
    }

    void versionVector::incrementGenOfPeer(peerID peer) {
        auto versP = findPeerIter(peer);
        version v;
        if (versP != _vers.end()) {
            v = *versP;
            v.gen++;
            _vers.erase(versP);
        } else {
            v.gen = 1;
            v.peer = copyPeerID(peer);
        }
        _vers.insert(_vers.begin(), v);
        _changed = true;
    }

    void versionVector::append(version vers) {
        vers.peer = copyPeerID(vers.peer);
        _vers.push_back(vers);
        _changed = true;
    }

    alloc_slice versionVector::copyPeerID(peerID peer) {
        return *_added.emplace(_added.begin(), peer);
    }


    // A hash table mapping peerID->generation, as an optimization for versionVector operations
    class versionMap {
    public:
        versionMap(const versionVector &vec) {
            _map.reserve(vec.count());
            for (auto v = vec._vers.begin(); v != vec._vers.end(); ++v)
                add(*v);
        }

        void add(const version &vers) {
            _map[vers.peer] = vers.gen;
        }

        generation operator[] (peerID peer) {
            auto i = _map.find(peer);
            return (i == _map.end()) ? 0 : i->second;
        }

    private:
        std::unordered_map<peerID, generation, sliceHash> _map;
    };
    
    
    versionVector versionVector::mergedWith(const versionVector &other) const {
        // Walk through the two vectors in parallel, adding the current component from each if it's
        // newer than the corresponding component in the other. This isn't going to produce the
        // optimal ordering, but it should be pretty close.
        versionMap myMap(*this), otherMap(other);
        versionVector result;
        for (size_t i = 0; i < std::max(_vers.size(), other._vers.size()); ++i) {
            peerID peer;
            if (i < _vers.size()) {
                auto &vers = _vers[i];
                auto othergen = otherMap[vers.peer];
                if (vers.gen >= othergen)
                    result.append(vers);
            }
            if (i < other._vers.size()) {
                auto &vers = other._vers[i];
                auto mygen = myMap[vers.peer];
                if (vers.gen > mygen)
                    result.append(vers);
            }
        }
        return result;
    }


}