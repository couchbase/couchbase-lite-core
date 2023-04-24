//
// URLTransformer.cc
//
//  Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "URLTransformer.hh"
#include "c4Replicator.hh"

namespace litecore::repl {
    URLTransformStrategy& operator++(URLTransformStrategy& s) {
        s = static_cast<URLTransformStrategy>(static_cast<unsigned>(s) + 1);
        return s;
    }

    static alloc_slice AsIs(slice inputURL) { return static_cast<alloc_slice>(inputURL); }

    static alloc_slice AddPort(slice inputURL) {
        C4Address addr;
        if ( !C4Address::fromURL(inputURL, &addr, nullptr) || (addr.port != 80 && addr.port != 443) ) {
            return nullslice;
        }

        if ( c4SliceEqual(kC4Replicator2Scheme, addr.scheme) ) {
            addr.port = 80;
        } else if ( c4SliceEqual(kC4Replicator2TLSScheme, addr.scheme) ) {
            addr.port = 443;
        }

        return addr.toURL();
    }

    static alloc_slice RemovePort(slice inputURL) {
        C4Address addr;
        if ( !C4Address::fromURL(inputURL, &addr, nullptr) || (addr.port != 80 && addr.port != 443) ) {
            return nullslice;
        }

        addr.port = 0;
        return addr.toURL();
    }

    alloc_slice transform_url(slice inputURL, URLTransformStrategy strategy) {
        switch ( strategy ) {
            case URLTransformStrategy::AsIs:
                return AsIs(inputURL);
            case URLTransformStrategy::AddPort:
                return AddPort(inputURL);
            case URLTransformStrategy::RemovePort:
                return RemovePort(inputURL);
        }

        return nullslice;
    }

    alloc_slice transform_url(const alloc_slice& inputURL, URLTransformStrategy strategy) {
        switch ( strategy ) {
            case URLTransformStrategy::AsIs:
                return inputURL;
            case URLTransformStrategy::AddPort:
                return AddPort(inputURL);
            case URLTransformStrategy::RemovePort:
                return RemovePort(inputURL);
        }

        return nullslice;
    }
}  // namespace litecore::repl
