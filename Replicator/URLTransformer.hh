//
// URLTransformer.hh
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

#pragma once

#include "fleece/slice.hh"
#include <optional>

namespace litecore::repl {
    using namespace fleece;
    using namespace std;

    /**
     * Strategies for handling the situation in CBL-1515
     * (briefly, platforms are inconsistent about including port number
     * in their URLs)
     */
    enum class URLTransformStrategy : unsigned {
        AsIs,       ///< Pass through the URL unaltered
        AddPort,    ///< Force the port in the URL
        RemovePort  ///< Force no port in the URL
    };

    // An operator to allow simple forward iteration on the enum values
    URLTransformStrategy& operator++(URLTransformStrategy& s);

    /**
     * A class for transforming URLs based on a provided strategy.  Note this
     * class will only function for URLs that are on either port 80
     * (standard HTTP) or 443 (standard TLS)
     */
    class URLTransformer {
    public:
        /**
         * Transforms the URL passed in input using the provided strategy
         *
         * @param input The URL to transform
         * @param strategy The strategy to use
         * @returns The transformed URL.  If the URL is not a candidate for AddPort or RemovePort
         *          (e.g. not a valid URL, or not using a standard port) then nullslice is returned)
         */
        static alloc_slice Transform(const slice &input, URLTransformStrategy strategy);

        /**
         * Transforms the URL passed in input using the provided strategy.  This overload
         * is simply to allow the optimization to not make copies in the AsIs strategy.
         *
         * @param input The URL to transform
         * @param strategy The strategy to use
         * @returns The transformed URL.  If the URL is not a candidate for AddPort or RemovePort
         *          (e.g. not a valid URL, or not using a standard port) then nullslice is returned)
         */
        static alloc_slice Transform(const alloc_slice &input, URLTransformStrategy strategy);
    };
}

