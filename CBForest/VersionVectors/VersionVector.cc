//
//  VersionVector.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "VersionVector.hh"
#include "SecureDigest.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "varint.hh"
#include <sstream>
#include <unordered_map>


namespace CBL_Core {


#pragma mark - VERSION:


    const peerID kCASServerPeerID = {"$", 1};
    const peerID kMePeerID        = {"*", 1};

    alloc_slice Version::peerIDFromBinary(slice binaryID) {
        fleece::Writer w;
        w.writeBase64(binaryID);
        return w.extractOutput();
    }

    Version::Version(slice string, bool validateAuthor) {
        if (string[0] == '^') {
            _gen = 0;
            _author = string;
            _author.moveStart(1);
            if (validateAuthor)
                validate();
        } else {
            _gen = string.readDecimal();                             // read generation
            if (_gen == 0 || string.readByte() != '@'                // read '@'
                         || string.size < 1 || string.size > kMaxAuthorSize)
                error::_throw(error::BadVersionVector);
            if (validateAuthor)
                if (_author.findByte(',') || _author.findByte('\0'))
                    error::_throw(error::BadVersionVector);
            _author = string;                                        // read peer ID
        }
    }

    void Version::validate() const {
        if (_author.size < 1 || _author.size > kMaxAuthorSize)
            error::_throw(error::BadVersionVector);
        if (isMerge()) {
            // Merge version must be valid base64:
            for (size_t i = 0; i < _author.size; i++) {
                char c = _author[i];
                if (!isalnum(c) && c != '+' && c != '/' && c != '=')
                    error::_throw(error::BadVersionVector);
            }
        } else {
            if (_author.findByte(',') || _author.findByte('\0'))
                error::_throw(error::BadVersionVector);
        }
    }

    generation Version::CAS() const {
        return _author == kCASServerPeerID ? _gen : 0;
    }

    alloc_slice Version::asString() const {
        if (isMerge()) {
            char buf[2 + _author.size];
            return alloc_slice(buf, sprintf(buf, "^%.*s", (int)_author.size, _author.buf));
        } else {
            char buf[30 + _author.size];
            return alloc_slice(buf, sprintf(buf, "%llu@%.*s", _gen, (int)_author.size, _author.buf));
        }
    }

    std::ostream& operator << (std::ostream &out, const Version &v) {
        if (v.isMerge())
            out << "^";
        else
            out << v.gen() << "@";
        out.write((const char*)v.author().buf, v.author().size);
        return out;
    }

    versionOrder Version::compareGen(generation a, generation b) {
        if (a > b)
            return kNewer;
        else if (a < b)
            return kOlder;
        return kSame;
    }

    versionOrder Version::compareTo(const VersionVector &vv) const {
        versionOrder o = vv.compareTo(*this);
        if (o == kOlder)
            return kNewer;
        else if (o == kNewer)
            return kOlder;
        else
            return o;
    }


#pragma mark - LIFECYCLE:


    VersionVector::VersionVector(slice string)
    :_string(string)
    {
        if (string.size == 0 || string.findByte('\0'))
            error::_throw(error::BadVersionVector);

        while (string.size > 0) {
            const void *comma = string.findByte(',') ?: string.end();
            _vers.push_back( Version(string.upTo(comma), false) );
            string = string.from(comma);
            if (string.size > 0)
                string.moveStart(1); // skip comma
        }
    }

    VersionVector::VersionVector(const fleece::Value* val) {
        readFrom(val);
    }

    VersionVector::VersionVector(const VersionVector &v) {
        *this = v;
    }

    VersionVector::VersionVector(VersionVector &&oldv)
    :_string(std::move(oldv._string)),
     _vers(std::move(oldv._vers)),
     _addedAuthors(std::move(oldv._addedAuthors))
    { }

    void VersionVector::reset() {
        _string = slice::null;
        _vers.clear();
        _addedAuthors.clear();
    }

    VersionVector& VersionVector::operator=(const VersionVector &v) {
        reset();
        for (auto vers : v._vers)
            append(vers);
        return *this;
    }


#pragma mark - CONVERSION:


    void VersionVector::readFrom(const fleece::Value *val) {
        reset();
        auto *arr = val->asArray();
        if (!arr)
            error::_throw(error::BadVersionVector);
        fleece::Array::iterator i(arr);
        if (i.count() % 2 != 0)
            error::_throw(error::BadVersionVector);
        for (; i; i += 2)
            _vers.push_back( Version((generation)i[0]->asUnsigned(), i[1]->asString()) );
    }


    void VersionVector::writeTo(fleece::Encoder &encoder) const {
        encoder.beginArray();
        for (auto v : _vers) {
            encoder << v._gen;
            encoder << v._author;
        }
        encoder.endArray();
    }


    std::string VersionVector::asString() const {
        return exportAsString(kMePeerID);   // leaves "*" unchanged
    }


    std::string VersionVector::exportAsString(peerID myID) const {
        std::stringstream out;
        for (auto v = _vers.begin(); v != _vers.end(); ++v) {
            if (v != _vers.begin())
                out << ",";
            if (v->_author == kMePeerID)
                out << Version(v->_gen, myID);
            else
                out << *v;
        }
        return out.str();
    }


    std::string VersionVector::canonicalString(peerID myPeerID) const {
        auto vec = *this; // copy before sorting
        vec.expandMyPeerID(myPeerID);
        std::sort(vec._vers.begin(), vec._vers.end(),
                  [myPeerID](const Version &a, const Version &b) {
                      return a._author < b._author;
                  });
        return vec.asString();
    }
    
    
#pragma mark - OPERATIONS:


    versionOrder VersionVector::compareTo(const Version& v) const {
        auto mine = const_cast<VersionVector*>(this)->findPeerIter(v._author);
        if (mine == _vers.end())
            return kOlder;
        else if (mine->_gen < v._gen)
            return kOlder;
        else if (mine->_gen == v._gen && mine == _vers.begin())
            return kSame;
        else
            return kNewer;
    }


    versionOrder VersionVector::compareTo(const VersionVector &other) const {
        int o = kSame;
        ssize_t countDiff = count() - other.count();
        if (countDiff < 0)
            o = kOlder;
        else if (countDiff > 0)
            o = kNewer;

        for (auto &v : _vers) {
            auto othergen = other[v._author];
            if (v._gen < othergen)
                o |= kOlder;
            else if (v._gen > othergen)
                o |= kNewer;
            else if (o == kSame)
                break; // first revs are identical so vectors are equal
            if (o == kConflicting)
                break;
        }
        return (versionOrder)o;
    }

    std::vector<Version>::iterator VersionVector::findPeerIter(peerID author) {
        auto v = _vers.begin();
        for (; v != _vers.end(); ++v) {
            if (v->_author == author)
                break;
        }
        return v;
    }

    generation VersionVector::genOfAuthor(peerID author) const {
        auto v = const_cast<VersionVector*>(this)->findPeerIter(author);
        return (v != _vers.end()) ? v->_gen : 0;
    }

    void VersionVector::incrementGen(peerID author) {
        auto versP = findPeerIter(author);
        Version v;
        if (versP != _vers.end()) {
            v = *versP;
            if (v.isMerge())
                error::_throw(error::BadVersionVector);
            v._gen++;
            _vers.erase(versP);
        } else {
            v._gen = 1;
            v._author = copyAuthor(author);
            v.validate();
        }
        _vers.insert(_vers.begin(), v);
    }

#pragma mark - MODIFICATION:


    void VersionVector::append(Version vers) {
        vers.validate();
        vers._author = copyAuthor(vers._author);
        _vers.push_back(vers);
    }

    alloc_slice VersionVector::copyAuthor(peerID author) {
        return *_addedAuthors.emplace(_addedAuthors.begin(), author);
    }

    void VersionVector::compactMyPeerID(peerID myID) {
        auto versP = findPeerIter(myID);
        if (versP != _vers.end())
            versP->_author = kMePeerID;
    }

    void VersionVector::expandMyPeerID(peerID myID) {
        auto versP = findPeerIter(kMePeerID);
        if (versP != _vers.end())
            versP->_author = copyAuthor(myID);
    }

    bool VersionVector::isExpanded() const {
        for (auto &vers : _vers)
            if (vers._author == kMePeerID)
                return false;
        return true;
    }


#pragma mark - MERGING:


    // A hash table mapping peerID->generation, as an optimization for versionVector operations
    class versionMap {
    public:
        versionMap(const VersionVector &vec) {
            _map.reserve(vec.count());
            for (auto &v : vec._vers)
                add(v);
        }

        void add(const Version &vers) {
            _map[vers.author()] = vers.gen();
        }

        generation operator[] (peerID author) {
            auto i = _map.find(author);
            return (i == _map.end()) ? 0 : i->second;
        }

    private:
        std::unordered_map<peerID, generation, fleece::sliceHash> _map;
    };
    
    
    VersionVector VersionVector::mergedWith(const VersionVector &other) const {
        // Walk through the two vectors in parallel, adding the current component from each if it's
        // newer than the corresponding component in the other. This isn't going to produce the
        // optimal ordering, but it should be pretty close.
        versionMap myMap(*this), otherMap(other);
        VersionVector result;
        for (size_t i = 0; i < std::max(_vers.size(), other._vers.size()); ++i) {
            peerID author;
            if (i < _vers.size()) {
                auto &vers = _vers[i];
                auto othergen = otherMap[vers._author];
                if (vers._gen >= othergen)
                    result.append(vers);
            }
            if (i < other._vers.size()) {
                auto &vers = other._vers[i];
                auto mygen = myMap[vers._author];
                if (vers._gen > mygen)
                    result.append(vers);
            }
        }
        return result;
    }


    void VersionVector::insertMergeRevID(peerID myPeerID, slice revisionBody) {
        // Merge ID is base64 of SHA-1 digest of version vector + nul byte + revision body
        sha1Context ctx;
        sha1_begin(&ctx);
        auto versString = canonicalString(myPeerID);
        sha1_add(&ctx, versString.data(), versString.size());
        sha1_add(&ctx ,"\0", 1);
        sha1_add(&ctx, revisionBody.buf, revisionBody.size);
        char digest[20];
        sha1_end(&ctx, digest);
        auto mergeID = Version::peerIDFromBinary(slice(&digest, sizeof(digest)));

        // Prepend a version representing the merge:
        Version mergeVers(0, copyAuthor(mergeID));
        _vers.insert(_vers.begin(), mergeVers);
    }


}
