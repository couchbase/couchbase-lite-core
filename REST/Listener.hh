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
#include <mutex>
#include <vector>

namespace litecore { namespace REST {
    class Request;
    class Server;

    class Listener {
    public:
        Listener(uint16_t port);
        ~Listener();

        /** Makes a database visible via the REST API. Or pass NULL to unregister. */
        void registerDatabase(std::string name, C4Database*);

        C4Database* databaseNamed(const std::string &name);
        std::vector<std::string> databaseNames();

    private:
        static void handleGetRoot(Request&);
        static void handleGetAllDBs(Request&);
        static void handleGetDatabase(Request&);
        static void handleGetAllDocs(Request&);
        static void handleGetDoc(Request&);

        std::mutex _mutex;
        std::unique_ptr<Server> _server;
        std::map<std::string, c4::ref<C4Database>> _databases;
    };

} }
