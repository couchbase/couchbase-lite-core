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
    class RequestResponse;

    class SyncListener : public HTTPListener {
      public:
        static constexpr int kAPIVersion = 2;

        explicit SyncListener(const Config&);
        ~SyncListener();

      private:
        class SyncTask;

        std::string findMatchingProtocol(DBShare const&, std::string_view clientProtocols);
        void        handleSync(RequestResponse&);

        bool const _allowPush, _allowPull;
        bool const _enableDeltaSync;
    };

}  // namespace litecore::REST

#endif
