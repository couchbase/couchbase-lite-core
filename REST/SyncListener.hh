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
#include <span>
#include <vector>

#ifdef COUCHBASE_ENTERPRISE

namespace litecore::REST {

    class SyncListener : public HTTPListener {
      public:
        static constexpr int kAPIVersion = 2;

        explicit SyncListener(const C4ListenerConfig&);
        ~SyncListener();

      protected:
        HTTPStatus handleRequest(Request& rq, websocket::Headers& headers,
                                 std::unique_ptr<ResponderSocket>& socket) override;

      private:
        class SyncTask;

        bool const _allowPush, _allowPull;
        bool const _enableDeltaSync;
    };

}  // namespace litecore::REST

#endif
