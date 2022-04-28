//
// QueryServer.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include <unordered_map>

namespace litecore::repl {

    class QueryServer final : public Worker {
    public:
        QueryServer(Replicator *replicator NONNULL);

        C4Query* getQuery(const std::string &name);
        
    private:
        void handleQuery(Retained<blip::MessageIn> request);

        std::unordered_map<string, Retained<C4Query>> _queries;
    };

}
