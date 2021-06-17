//
// URLTransformer.cc
//
//  Copyright (c) 2021 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "URLTransformer.hh"
#include "c4Replicator.hh"

namespace litecore::repl {
    URLTransformStrategy& operator++(URLTransformStrategy& s)  {
        s = static_cast<URLTransformStrategy>(static_cast<unsigned>(s) + 1);
        return s;
    }

    static alloc_slice AsIs(slice inputURL) {
        return static_cast<alloc_slice>(inputURL);
    }

    static alloc_slice AddPort(slice inputURL) {
        C4Address addr;
        if(!C4Address::fromURL(inputURL, &addr, nullptr) || (addr.port != 80 && addr.port != 443)) {
            return nullslice;
        }

        if(c4SliceEqual(kC4Replicator2Scheme, addr.scheme)) {
            addr.port = 80;
        } else if(c4SliceEqual(kC4Replicator2TLSScheme, addr.scheme)) {
            addr.port = 443;
        }

        return addr.toURL();
    }

    static alloc_slice RemovePort(slice inputURL) {
        C4Address addr;
        if(!C4Address::fromURL(inputURL, &addr, nullptr) || (addr.port != 80 && addr.port != 443)) {
            return nullslice;
        }

        addr.port = 0;
        return addr.toURL();
    }

    alloc_slice transform_url(slice inputURL, URLTransformStrategy strategy) {
        switch(strategy) {
        case URLTransformStrategy::AsIs:
            return AsIs(inputURL);
        case URLTransformStrategy::AddPort:
            return AddPort(inputURL);
        case URLTransformStrategy::RemovePort:
            return RemovePort(inputURL);
        }

        return nullslice;
    }

    alloc_slice transform_url(const alloc_slice &inputURL, URLTransformStrategy strategy) {
        switch(strategy) {
        case URLTransformStrategy::AsIs:
            return inputURL;
        case URLTransformStrategy::AddPort:
            return AddPort(inputURL);
        case URLTransformStrategy::RemovePort:
            return RemovePort(inputURL);
        }

        return nullslice;
    }
}
