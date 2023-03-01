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
#include "fleece/slice.hh"

namespace litecore { namespace REST {

    std::string URLDecode(fleece::slice str, bool isFormURLEncoded = true);

    std::string URLEncode(fleece::slice str);

    std::string getURLQueryParam(fleece::slice queries, const char* name, char delimiter = '&', size_t occurrence = 0);

}}  // namespace litecore::REST
