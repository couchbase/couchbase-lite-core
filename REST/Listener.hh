//
//  Listener.hh
//  LiteCore
//
//  Created by Jens Alfke on 5/22/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4.hh"
#include "c4Listener.h"
#include "FilePath.hh"
#include <map>
#include <mutex>
#include <vector>

namespace litecore { namespace REST {

    /** Abstract superclass of network listeners that can serve access to databases.
        Subclassed by RESTListener and SyncListener. */
    class Listener {
    public:
        using Config = C4ListenerConfig;

        virtual ~Listener() =default;

        /** Determines whether a database name is valid for use as a URI path component.
            It must be nonempty, no more than 240 bytes long, not start with an underscore,
            and contain no control characters. */
        static bool isValidDatabaseName(const std::string&);

        /** Given a filesystem path to a database, returns the database name.
            (This takes the last path component and removes the ".cblite2" extension.)
            Returns an empty string if the path is not a database, or if the name would not
            be valid according to isValidDatabaseName(). */
        static std::string databaseNameFromPath(const FilePath&);

        /** Makes a database visible via the REST API.
            Retains the C4Database; the caller does not need to keep a reference to it. */
        bool registerDatabase(std::string name, C4Database*);

        /** Unregisters a database by name.
            The C4Database will be closed if there are no other references to it. */
        bool unregisterDatabase(std::string name);

        /** Returns the database registered under the given name. */
        c4::ref<C4Database> databaseNamed(const std::string &name);

        /** Returns all registered database names. */
        std::vector<std::string> databaseNames();

    protected:
        static constexpr uint16_t kDefaultPort = 4984;

        Listener();

        std::mutex _mutex;
        std::map<std::string, c4::ref<C4Database>> _databases;
    };

} }
