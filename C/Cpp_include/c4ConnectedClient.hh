//
// c4ConnectedClient.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "Async.hh"
#include "C4ConnectedClient.h"

C4_ASSUME_NONNULL_BEGIN

struct C4ConnectedClient  : public fleece::RefCounted,
                            public fleece::InstanceCountedIn<C4Database>,
                            C4Base  {
    
    static Retained<C4ConnectedClient> newClient(const C4ConnectedClientParameters &params);
                                
    virtual litecore::actor::Async<C4DocResponse> getDoc(C4Slice, C4Slice, C4Slice, bool) noexcept=0;
};

C4_ASSUME_NONNULL_END
