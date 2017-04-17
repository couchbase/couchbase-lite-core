//
//  Listener.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4.hh"
#include <map>
#include <memory>
#include <vector>

namespace litecore { namespace REST {
    class Server;

    class Listener {
    public:
        Listener(uint16_t port);
        ~Listener();

        /** Makes a database visible via the REST API. Or pass NULL to unregister. */
        void registerDatabase(std::string name, C4Database*);

    private:
        std::unique_ptr<Server> _server;
        std::map<std::string, c4::ref<C4Database>> _databases;
    };

} }
