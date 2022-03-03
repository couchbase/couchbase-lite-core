//
// StringUtil_JS.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "StringUtil.hh"
#include "emscripten/val.h"

namespace litecore {
    using namespace fleece;

    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        auto strVal = emscripten::val(str.asString());
        std::string resultStr;
        if (toUppercase){
            resultStr = strVal.call<std::string>("toUpperCase");
        } else {
            resultStr = strVal.call<std::string>("toLowerCase");
        }
        return alloc_slice(resultStr);
    }
}