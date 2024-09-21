//
// netUtils.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/function_ref.hh"
#include "fleece/slice.hh"

namespace litecore::REST {

    std::string URLDecode(fleece::slice str, bool isFormURLEncoded = true);

    std::string URLEncode(fleece::slice str);

    /// Gets a URL query parameter by name. The returned value is URL-decoded.
    std::string getURLQueryParam(fleece::slice queries, std::string_view name, char delimiter = '&',
                                 size_t occurrence = 0);

    /// Calls the callback with the name and (raw) value of each query parameter.
    /// @param queries  The query string (without the initial '?')
    /// @param delimiter  The character separating queries, usually '&'
    /// @param callback  A function that will be passed the name and (raw) value.
    ///                  You need to call \ref URLDecode to decode the value.
    ///                  Return `false` to continue the iteration, `true` to stop.
    /// @returns  true if the callback stopped the iteration, else false.
    bool iterateURLQueries(std::string_view queries, char delimiter,
                           fleece::function_ref<bool(std::string_view, std::string_view)> callback);

}  // namespace litecore::REST
