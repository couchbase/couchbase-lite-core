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
#include "c4Replicator.h"

namespace litecore::repl {
    URLTransformStrategy& operator++(URLTransformStrategy& s)  {
        s = static_cast<URLTransformStrategy>(static_cast<unsigned>(s) + 1);
        return s;
    }


    typedef alloc_slice(*URLStrategyFunction)(const slice &input);

    static alloc_slice AsIs(const slice &input) {
        return static_cast<alloc_slice>(input);
    }

    static alloc_slice AddPort(const slice &input) {
        C4Address addr;
        if(!c4address_fromURL(input, &addr, nullptr) || (addr.port != 80 && addr.port != 443)) {
            return nullslice;
        }

        if(c4SliceEqual(kC4Replicator2Scheme, addr.scheme)) {
            addr.port = 80;
        } else if(c4SliceEqual(kC4Replicator2TLSScheme, addr.scheme)) {
            addr.port = 443;
        }

        return c4address_toURL(addr);
    }

    static alloc_slice RemovePort(const slice &input) {
        C4Address addr;
        if(!c4address_fromURL(input, &addr, nullptr) || (addr.port != 80 && addr.port != 443)) {
            return nullslice;
        }

        addr.port = 0;
        return c4address_toURL(addr);
    }

    static URLStrategyFunction sStrategies[] = { AsIs, AddPort, RemovePort };
    /* static */ alloc_slice URLTransformer::Transform(const slice &input, URLTransformStrategy strategy) {
        return sStrategies[static_cast<int>(strategy)](input);
    }

    /* static */ alloc_slice URLTransformer::Transform(const alloc_slice &input, URLTransformStrategy strategy) {
        if(strategy == URLTransformStrategy::AsIs) {
            return input;
        }

        return sStrategies[static_cast<int>(strategy)](input);
    }
}
