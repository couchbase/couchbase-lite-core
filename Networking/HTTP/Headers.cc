//
// Headers.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Headers.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"
#include "slice_stream.hh"
#include <string.h>
#include "betterassert.hh"

namespace litecore { namespace websocket {
    using namespace fleece;

    Headers::Headers(fleece::alloc_slice encoded)
    :_backingStore(encoded)
    {
        readFrom(ValueFromData(encoded).asDict());
    }


    Headers::Headers(Dict dict) {
        readFrom(dict);
    }


    Headers::Headers(const Headers &other) {
        *this = other;
    }


    Headers::Headers(Headers &&other)
    :_map(std::move(other._map))
    ,_backingStore(std::move(other._backingStore))
    ,_writer(std::move(other._writer))
    { }


    Headers& Headers::operator= (const Headers &other) {
        clear();
        if (other._writer.length() == 0) {
            _map = other._map;
            _backingStore = other._backingStore;
        } else {
            setBackingStore(other._backingStore);
            for (auto &entry : other._map)
                add(entry.first, entry.second);
        }
        return *this;
    }


    void Headers::readFrom(Dict dict) {
        for (Dict::iterator i(dict); i; ++i) {
            slice key = i.keyString();
            Array multiple = i.value().asArray();
            if (multiple) {
                for (Array::iterator j(multiple); j; ++j)
                    add(key, j.value().asString());
            } else {
                add(key, i.value().asString());
            }
        }
    }


    void Headers::setBackingStore(alloc_slice backingStore) {
        assert(_map.empty());
        _backingStore = backingStore;
    }
    
    
    void Headers::clear() {
        _map.clear();
        _backingStore = nullslice;
        _writer.reset();
    }
    
    
    slice Headers::store(slice s) {
        if (_backingStore.containsAddressRange(s))
            return s;
        return slice(_writer.write(s), s.size);
    }


    void Headers::add(slice name, slice value) {
        assert(name);
        if (value)
            _map.insert({store(name), store(value)});
    }


    slice Headers::get(slice name) const {
        auto i = _map.find(name);
        if (i == _map.end())
            return nullslice;
        return i->second;
    }


    int64_t Headers::getInt(slice name, int64_t defaultValue) const {
        slice_istream v = get(name);
        if (!v)
            return defaultValue;
        int64_t n = v.readSignedDecimal();
        if (v.size > 0)
            return defaultValue;
        return n;
    }


    void Headers::forEach(fleece::function_ref<void(slice,slice)> callback) const {
        for (auto i = _map.begin(); i != _map.end(); ++i)
            callback(i->first, i->second);
    }


    void Headers::forEach(slice name, fleece::function_ref<void(slice)> callback) const {
        auto end = _map.upper_bound(name);
        for (auto i = _map.lower_bound(name); i != end; ++i)
            callback(i->second);
    }


    alloc_slice Headers::encode() const {
        Encoder enc;
        enc.beginDict();
        auto end = _map.end();
        for (auto i = _map.begin(); i != end; ++i) {
            slice key = i->first;
            enc.writeKey(key);
            if (next(i) != end && next(i)->first == key) {
                // If there are duplicate keys, write their values as an array:
                enc.beginArray();
                while (next(i) != end && next(i)->first == key) {
                    enc.writeString(i->second);
                    ++i;
                }
                enc.endArray();
            } else {
                enc.writeString(i->second);
            }
        }
        enc.endDict();
        return enc.finish();
    }
    

} }
