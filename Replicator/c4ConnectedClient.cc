//
// c4ConnectedClient.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Base.hh"
#include "c4ConnectedClient.hh"
#include "c4ConnectedClientImpl.hh"
#include "ConnectedClient.hh"

using namespace litecore::client;
using namespace fleece;

/*static*/ Retained<C4ConnectedClient> C4ConnectedClient::newClient(const C4ConnectedClientParameters &params) {
    try {
        return new C4ConnectedClientImpl(params);
    } catch (...) {
        throw;
    }
}

