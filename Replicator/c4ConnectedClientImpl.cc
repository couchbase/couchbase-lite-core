//
// c4ConnectedClientImpl.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


#include "c4ConnectedClientImpl.hh"
#include "Replicator.hh"

using namespace fleece;

namespace litecore::client {

    alloc_slice C4ConnectedClientImpl::effectiveURL(slice url) {
        string newPath = string(url);
        if (!url.hasSuffix("/"_sl))
            newPath += "/";
        newPath += "_blipsync";
        return alloc_slice(newPath);
    }

    alloc_slice C4ConnectedClientImpl::socketOptions() {
        fleece::Encoder enc;
        enc.beginDict();
        enc.writeKey(C4STR(kC4SocketOptionWSProtocols));
        enc.writeString(repl::Replicator::ProtocolName().c_str());
        enc.endDict();
        return enc.finish();
    }
}
