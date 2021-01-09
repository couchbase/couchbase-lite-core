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
     * Transforms the URL passed in input using the provided strategy (note only URLs using
     * standard ports 80 / 443 will be considered for transformation)
     *
     * @param input The URL to transform
     * @param strategy The strategy to use
     * @returns The transformed URL.  If the URL is not a candidate for AddPort or RemovePort
     *          (e.g. not a valid URL, or not using a standard port) then nullslice is returned)
     */
    alloc_slice transform_url(slice inputURL, URLTransformStrategy strategy);

    /**
     * Transforms the URL passed in input using the provided strategy (note only URLs using
     * standard ports 80 / 443 will be considered for transformation).  This overload
     * is simply to allow the optimization to not make copies in the AsIs strategy.
     *
     * @param input The URL to transform
     * @param strategy The strategy to use
     * @returns The transformed URL.  If the URL is not a candidate for AddPort or RemovePort
     *          (e.g. not a valid URL, or not using a standard port) then nullslice is returned)
     */
    alloc_slice transform_url(const alloc_slice &inputURL, URLTransformStrategy strategy);
}

