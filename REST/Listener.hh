//
// Listener.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "RefCounted.hh"
#include "InstanceCounted.hh"
#include "c4CppUtils.hh"
#include "c4Listener.h"
#include "FilePath.hh"
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace litecore { namespace REST {

    /** Abstract superclass of network listeners that can serve access to databases.
        Subclassed by RESTListener. */
    class Listener : public fleece::RefCounted, public fleece::InstanceCountedIn<Listener> {
    public:
        using Config = C4ListenerConfig;

        static constexpr uint16_t kDefaultPort = 4984;

        Listener(const Config &config);
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
        bool registerDatabase(C4Database* NONNULL, std::optional<std::string> name =std::nullopt);

        /** Unregisters a database by name.
            The C4Database will be closed if there are no other references to it. */
        bool unregisterDatabase(std::string name);

        bool unregisterDatabase(C4Database *db);

        /** Returns the database registered under the given name. */
        c4::ref<C4Database> databaseNamed(const std::string &name) const;

        /** Returns the name a database is registered under. */
        std::optional<std::string> nameOfDatabase(C4Database* NONNULL) const;

        /** Returns all registered database names. */
        std::vector<std::string> databaseNames() const;

        /** Returns the number of client connections. */
        virtual int connectionCount() =0;

        /** Returns the number of active client connections (for some definition of "active"). */
        virtual int activeConnectionCount() =0;

    protected:
        mutable std::mutex _mutex;
        Config _config;
        std::map<std::string, c4::ref<C4Database>> _databases;
    };

} }
