//
// Headers.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include "fleece/function_ref.hh"
#include <map>
#include <vector>

namespace fleece {
    class Dict;
}

namespace litecore::websocket {


    /** HTTP headers. A specialized map that has case-insensitive keys and allows multiple
        occurrences of a key. */
    class Headers {
      public:
        using slice       = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        /** Creates an empty instance. */
        Headers() = default;

        /** Instantiate from a Fleece Dict whose keys are header names and values are either
            strings or arrays of strings. */
        explicit Headers(fleece::Dict);

        /** Reconstitute from an encoded Fleece Dict. */
        explicit Headers(const alloc_slice& encoded);

        /** Reconstitute from an encoded Fleece Dict. */
        explicit Headers(slice encoded) : Headers(alloc_slice(encoded)) {}

        Headers(const Headers&)                = default;
        Headers& operator=(const Headers&)     = default;
        Headers(Headers&&) noexcept            = default;
        Headers& operator=(Headers&&) noexcept = default;

        /** Removes all headers. */
        void clear();

        /** True if there are no headers. */
        [[nodiscard]] bool empty() const { return _map.empty(); }

        /** Adds a header. If a header with that name already exists, it adds a second. */
        void add(slice name, slice value);

        /** Sets the value of a header. If headers with that name exist, they're replaced. */
        void set(slice name, slice value);

        /** Returns the value of a header with that name.*/
        [[nodiscard]] slice get(slice name) const;

        /** Returns a header parsed as an integer. If missing, returns `defaultValue` */
        [[nodiscard]] int64_t getInt(slice name, int64_t defaultValue = 0) const;

        /** Returns the value of a header with that name. */
        [[nodiscard]] slice operator[](slice name) const { return get(name); }

        /** Returns all header values with the given name, separated by commas. */
        [[nodiscard]] std::string getAll(slice name) const;

        /** Calls the function once for each header/value pair, in ASCII order.*/
        void forEach(fleece::function_ref<void(slice, slice)> callback) const;

        /** Calls the function once for each header with the given name.*/
        void forEach(slice name, fleece::function_ref<void(slice)> callback) const;

        /** Encodes the headers as a Fleece dictionary. Each key is a header name, and its
            value is a string if it's unique, or an array of strings if multiple. */
        [[nodiscard]] alloc_slice encode() const;

      private:
        void  readFrom(fleece::Dict);
        slice store(slice s);

        class HeaderCmp {
          public:
            bool operator()(slice a, slice b) const noexcept { return a.caseEquivalentCompare(b) < 0; }
        };

        std::multimap<slice, slice, HeaderCmp> _map;
        std::vector<alloc_slice>               _backingStore;  // Owns the data that _map points to
    };


}  // namespace litecore::websocket
