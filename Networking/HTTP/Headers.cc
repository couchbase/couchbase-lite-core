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
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"
#include "slice_stream.hh"
#include <cstring>

#include "betterassert.hh"

namespace litecore::websocket {
    using namespace std;
    using namespace fleece;

    Headers::Headers(const fleece::alloc_slice& encoded) : _backingStore{encoded} {
        readFrom(ValueFromData(encoded).asDict());
    }

    Headers::Headers(Dict dict) { readFrom(dict); }

    void Headers::readFrom(Dict dict) {
        for ( Dict::iterator i(dict); i; ++i ) {
            slice key      = i.keyString();
            Array multiple = i.value().asArray();
            if ( multiple ) {
                for ( Array::iterator j(multiple); j; ++j ) add(key, j.value().asString());
            } else {
                add(key, i.value().asString());
            }
        }
    }

    void Headers::clear() {
        _map.clear();
        _backingStore.clear();
    }

    slice Headers::store(slice s) {
        for ( auto& stored : _backingStore ) {
            if ( stored.containsAddressRange(s) ) return s;
        }
        _backingStore.emplace_back(s);
        return _backingStore.back();
    }

    void Headers::add(slice name, slice value) {
        assert(name);
        if ( value ) _map.insert({store(name), store(value)});
    }

    void Headers::set(slice name, slice value) {
        assert(name);
        _map.erase(name);
        add(name, value);
    }

    slice Headers::get(slice name) const {
        auto i = _map.find(name);
        if ( i == _map.end() ) return nullslice;
        return i->second;
    }

    int64_t Headers::getInt(slice name, int64_t defaultValue) const {
        slice_istream v = get(name);
        if ( !v ) return defaultValue;
        int64_t n = v.readSignedDecimal();
        if ( v.size > 0 ) return defaultValue;
        return n;
    }

    std::string Headers::getAll(slice name) const {
        string all;
        forEach(name, [&all](slice value) {
            if ( !all.empty() ) all += ',';
            all += string_view(value);
        });
        return all;
    }

    void Headers::forEach(fleece::function_ref<void(slice, slice)> callback) const {
        for ( const auto& i : _map ) callback(i.first, i.second);
    }

    void Headers::forEach(slice name, fleece::function_ref<void(slice)> callback) const {
        auto end = _map.upper_bound(name);
        for ( auto i = _map.lower_bound(name); i != end; ++i ) callback(i->second);
    }

    alloc_slice Headers::encode() const {
        Encoder enc;
        enc.beginDict();
        auto end = _map.end();
        for ( auto i = _map.begin(); i != end; ++i ) {
            slice key = i->first;
            enc.writeKey(key);
            if ( next(i) != end && next(i)->first == key ) {
                // If there are duplicate keys, write their values as an array:
                enc.beginArray();
                while ( next(i) != end && next(i)->first == key ) {
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


}  // namespace litecore::websocket
