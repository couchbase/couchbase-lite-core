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
#include "c4Listener.hh"
#include "fleece/RefCounted.hh"

namespace litecore { namespace REST {
    class Listener;


    extern C4LogDomain ListenerLog;


    extern const C4ListenerAPIs kListenerAPIs;
    fleece::Retained<Listener>  NewListener(const C4ListenerConfig* config);


}}  // namespace litecore::REST
