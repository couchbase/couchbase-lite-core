//
// c4ListenerInternal.hh
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
#include "fleece/RefCounted.hh"
#include "c4Log.h"
#include "c4ListenerTypes.h"

namespace litecore::REST {
    class HTTPListener;

    extern C4LogDomain ListenerLog;

}  // namespace litecore::REST
