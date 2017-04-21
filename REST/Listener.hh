//
//  Listener.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4.hh"
#include "c4REST.h"
#include "FilePath.hh"
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace litecore { namespace REST {
    class Request;
    class Server;

    class Listener {
    public:
        using Config = C4RESTConfig;

        Listener(const Config&);
        ~Listener();

        static bool isValidDatabaseName(const std::string&);
        static std::string databaseNameFromPath(const FilePath&);
        bool pathFromDatabaseName(const std::string &name, FilePath &outPath);

        /** Makes a database visible via the REST API. */
        bool registerDatabase(std::string name, C4Database*);

        /** Unregisters a database by name. 
            The C4Database will be closed if there are no other references to it. */
        bool unregisterDatabase(std::string name);

        /** Opens a database and makes it visible via the REST API.
            If the name is an empty string, a default name will be used based on the
            filename. */
        bool openDatabase(std::string name,
                          const FilePath&,
                          const C4DatabaseConfig*,
                          C4Error*);

        /** Returns the database registered under the given name. */
        c4::ref<C4Database> databaseNamed(const std::string &name);

        std::vector<std::string> databaseNames();

    private:
        /** Returns the database for this request, or null on error. */
        c4::ref<C4Database> databaseFor(Request&);
        
        void handleGetRoot(Request&);
        void handleGetAllDBs(Request&);

        void handleGetDatabase(Request&);
        void handleCreateDatabase(Request&);
        void handleDeleteDatabase(Request&);

        void handleGetAllDocs(Request&);
        void handleGetDoc(Request&);
        void handleModifyDoc(Request&);

        std::unique_ptr<FilePath> _directory;
        const bool _allowCreateDB, _allowDeleteDB;
        std::unique_ptr<Server> _server;
        std::mutex _mutex;
        std::map<std::string, c4::ref<C4Database>> _databases;
    };

} }
