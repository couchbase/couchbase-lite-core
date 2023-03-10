//
// URLTransformer.hh
//
//  Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
     * @param inputURL The URL to transform
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
     * @param inputURL The URL to transform
     * @param strategy The strategy to use
     * @returns The transformed URL.  If the URL is not a candidate for AddPort or RemovePort
     *          (e.g. not a valid URL, or not using a standard port) then nullslice is returned)
     */
    alloc_slice transform_url(const alloc_slice& inputURL, URLTransformStrategy strategy);
}  // namespace litecore::repl
