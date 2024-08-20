//
// SyncListener.hh
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
#include "HTTPListener.hh"
#include "Address.hh"
#include "c4ReplicatorTypes.h"
#include <vector>

#ifdef COUCHBASE_ENTERPRISE

namespace litecore::REST {
    class RequestResponse;

    class SyncListener : public HTTPListener {
      public:
        SyncListener(const Config&);
        ~SyncListener();

        virtual int activeConnectionCount() override;

        void stop() override;

      private:
        void handleSync(RequestResponse&, C4Database*);
        void replicatorStatusChanged(C4Replicator* repl, C4ReplicatorStatus status);

        bool const                                  _allowPush, _allowPull, _enableDeltaSync;
        std::vector<fleece::Retained<C4Replicator>> _replicators;
    };

}  // namespace litecore::REST

#endif
