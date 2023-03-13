//
//  n1ql_parser.hh
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
#include "fleece/Fleece.h"
#include <string>

namespace litecore { namespace n1ql {

    // Entry point of the N1QL parser (implementation at the bottom of n1ql.leg)
    FLMutableDict parse(const std::string& input, unsigned* errPos);

}}  // namespace litecore::n1ql
