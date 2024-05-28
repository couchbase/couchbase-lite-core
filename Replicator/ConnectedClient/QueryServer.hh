//
// QueryServer.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include <unordered_map>

namespace litecore::repl {

    /** Handles Connected-Client `query` requests for a passive Replicator. */
    class QueryServer final : public Worker {
      public:
        explicit QueryServer(Replicator* replicator NONNULL);

        C4Query*          getNamedQuery(const std::string& name);
        Retained<C4Query> compileQuery(slice queryStr);

      private:
        void handleQuery(Retained<blip::MessageIn> request);

        std::unordered_map<std::string, Retained<C4Query>> _queries;
    };

}  // namespace litecore::repl
