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
#include "Writer.hh"
#include "fleece/function_ref.hh"
#include <map>

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

        Headers() = default;

        /** Reconstitute from Fleece data. */
        explicit Headers(const alloc_slice& encoded);

        explicit Headers(slice encoded) : Headers(alloc_slice(encoded)) {}

        explicit Headers(fleece::Dict);

        Headers(const Headers&);
        Headers& operator=(const Headers&);
        Headers(Headers&&) noexcept;
        Headers& operator=(Headers&&) noexcept;

        void clear();

        [[nodiscard]] bool empty() const { return _map.empty(); }

        /** Keep a reference to this alloc_slice; any keys/values that are added that point
            within the backing store won't cause any allocation. */
        void setBackingStore(alloc_slice);

        /** Adds a header. If a header with that name already exists, it adds a second. */
        void add(slice name, slice value);

        /** Sets the value of a header. If headers with that name exist, they're replaced. */
        void set(slice name, slice value);

        /** Returns the value of a header with that name.*/
        [[nodiscard]] slice get(slice name) const;

        /** Returns a header parsed as an integer. If missing, returns `defaultValue` */
        [[nodiscard]] int64_t getInt(slice name, int64_t defaultValue = 0) const;

        /** Returns the comma-delimited component of a header, with leading/trailing space removed. */
        [[nodiscard]] std::vector<slice> getCommaSeparated(slice name) const;

        /** Returns the value of a header with that name.*/
        slice operator[](slice name) const { return get(name); }

        /** Calls the function once for each header, in ASCII order.*/
        void forEach(fleece::function_ref<void(slice, slice)> callback) const;

        /** Calls the function once for each value with the given name.*/
        void forEach(slice name, fleece::function_ref<void(slice)> callback) const;

        /** Encodes the headers as a Fleece dictionary. Each key is a header name, and its
            value is a string if it's unique, or an array of strings if multiple. */
        [[nodiscard]] alloc_slice encode() const;

      private:
        void  readFrom(fleece::Dict);
        slice store(slice s);

        class HeaderCmp {
          public:
            bool operator()(fleece::slice a, fleece::slice b) const noexcept { return a.caseEquivalentCompare(b) < 0; }
        };

        std::multimap<slice, slice, HeaderCmp> _map;
        alloc_slice                            _backingStore;
        fleece::Writer                         _writer;
    };


}  // namespace litecore::websocket
