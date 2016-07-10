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
#include "Fleece.hh"
#include <sstream>
#include <unordered_map>


namespace cbforest {

    const peerID kCASServerPeerID = {"$", 1};
    const peerID kMePeerID        = {"*", 1};

    version::version(slice string, bool validateAuthor) {
        gen = string.readDecimal();                             // read generation
        if (gen == 0 || string.readByte() != '@'                // read '@'
                     || string.size < 1 || string.size > kMaxAuthorSize)
            error::_throw(error::BadVersionVector);
        if (validateAuthor)
            if (author.findByte(',') || author.findByte('\0'))
                error::_throw(error::BadVersionVector);
        author = string;                                        // read peer ID
    }

    void version::validate() const {
        if (gen == 0 || author.size < 1 || author.size > kMaxAuthorSize
                     || author.findByte(',') || author.findByte('\0'))
            error::_throw(error::BadVersionVector);
    }

    generation version::CAS() const {
        return author == kCASServerPeerID ? gen : 0;
    }

    alloc_slice version::asString() const {
        char buf[30 + author.size];
        return alloc_slice(buf, sprintf(buf, "%llu@%.*s", gen, (int)author.size, author.buf));
    }


    versionVector::versionVector(slice string)
    :_string(string)
    {
        if (string.size == 0 || string.findByte('\0'))
            error::_throw(error::BadVersionVector);

        while (string.size > 0) {
            const void *comma = string.findByte(',') ?: string.end();
            _vers.push_back( version(string.upTo(comma), false) );
            string = string.from(comma);
            if (string.size > 0)
                string.moveStart(1); // skip comma
        }
    }

    versionVector::versionVector(const fleece::Value* val) {
        readFrom(val);
    }

    void versionVector::readFrom(const fleece::Value *val) {
        CBFDebugAssert(_vers.empty());
        auto *arr = val->asArray();
        if (!arr)
            error::_throw(error::BadVersionVector);
        fleece::Array::iterator i(arr);
        if (i.count() % 2 != 0)
            error::_throw(error::BadVersionVector);
        for (; i; i += 2)
            _vers.push_back( version((generation)i[1]->asUnsigned(), i[0]->asString()) );
    }

    alloc_slice versionVector::asString() const {
        if (!_changed && _string.buf)
            return _string;

        size_t dataSize = 0;
        for (auto v = _vers.begin(); v != _vers.end(); ++v)
            dataSize += slice::sizeOfDecimal(v->gen) + v->author.size + 2;
        if (dataSize > 0)
            --dataSize;

        alloc_slice data(dataSize);
        slice out = data;
        for (auto v = _vers.begin(); v != _vers.end(); ++v) {
            if (v != _vers.begin())
                out.writeByte(',');
            out.writeDecimal(v->gen);
            out.writeByte('@');
            out.writeFrom(v->author);
        }
        return data;
    }

    void versionVector::writeTo(fleece::Encoder &encoder) const {
        encoder.beginArray();
        for (auto v : _vers) {
            encoder << v.author;
            encoder << v.gen;
        }
        encoder.endArray();
    }

    versionVector::order versionVector::compareTo(const versionVector &other) const {
        int o = kSame;
        ssize_t countDiff = count() - other.count();
        if (countDiff < 0)
            o = kOlder;
        else if (countDiff > 0)
            o = kNewer;

        for (auto v = _vers.begin(); v != _vers.end(); ++v) {
            auto othergen = other[v->author];
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

    bool versionVector::isFromCASServer() const {
        return current().CAS() > 0;
    }

    std::vector<version>::iterator versionVector::findPeerIter(peerID author) {
        auto v = _vers.begin();
        for (; v != _vers.end(); ++v) {
            if (v->author == author)
                break;
        }
        return v;
    }

    generation versionVector::genOfAuthor(peerID author) const {
        auto v = const_cast<versionVector*>(this)->findPeerIter(author);
        return (v != _vers.end()) ? v->gen : 0;
    }

    void versionVector::incrementGen(peerID author) {
        auto versP = findPeerIter(author);
        version v;
        if (versP != _vers.end()) {
            v = *versP;
            v.gen++;
            _vers.erase(versP);
        } else {
            v.gen = 1;
            v.author = copyAuthor(author);
            v.validate();
        }
        _vers.insert(_vers.begin(), v);
        _changed = true;
    }

    bool versionVector::setCAS(generation cas) {
        CBFAssert(cas > 0);
        auto versP = findPeerIter(kCASServerPeerID);
        version v;
        if (versP != _vers.end()) {
            v = *versP;
            if (v.gen >= cas)
                return false;
            _vers.erase(versP);
        } else {
            v.author = kCASServerPeerID;
        }
        v.gen = cas;
        _vers.insert(_vers.begin(), v);
        _changed = true;
        return true;
    }

    void versionVector::append(version vers) {
        vers.validate();
        vers.author = copyAuthor(vers.author);
        _vers.push_back(vers);
        _changed = true;
    }

    alloc_slice versionVector::copyAuthor(peerID author) {
        return *_addedAuthors.emplace(_addedAuthors.begin(), author);
    }

    void versionVector::compactMyPeerID(peerID myID) {
        auto versP = findPeerIter(myID);
        if (versP != _vers.end()) {
            versP->author = kMePeerID;
            _changed = true;
        }
    }

    void versionVector::expandMyPeerID(peerID myID) {
        auto versP = findPeerIter(kMePeerID);
        if (versP != _vers.end()) {
            versP->author = copyAuthor(myID);
            _changed = true;
        }
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
            _map[vers.author] = vers.gen;
        }

        generation operator[] (peerID author) {
            auto i = _map.find(author);
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
            peerID author;
            if (i < _vers.size()) {
                auto &vers = _vers[i];
                auto othergen = otherMap[vers.author];
                if (vers.gen >= othergen)
                    result.append(vers);
            }
            if (i < other._vers.size()) {
                auto &vers = other._vers[i];
                auto mygen = myMap[vers.author];
                if (vers.gen > mygen)
                    result.append(vers);
            }
        }
        return result;
    }


}