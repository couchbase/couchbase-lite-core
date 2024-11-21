//
// Listener.hh
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
#include "fleece/RefCounted.hh"
#include "fleece/InstanceCounted.hh"
#include "c4Database.hh"
#include "c4ListenerTypes.h"
#include "FilePath.hh"
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#ifdef COUCHBASE_ENTERPRISE

namespace litecore {
    class DatabasePool;
    class BorrowedCollection;
    class BorrowedDatabase;
}  // namespace litecore

namespace litecore::REST {

    /** Abstract superclass of network listeners that can serve access to databases.
        Subclassed by HTTPListener. */
    class Listener
        : public fleece::RefCounted
        , public fleece::InstanceCountedIn<Listener> {
      public:
        using Config         = C4ListenerConfig;
        using DatabaseConfig = C4ListenerDatabaseConfig;
        using CollectionSpec = C4Database::CollectionSpec;

        static constexpr uint16_t kDefaultPort = 4984;

        explicit Listener(const Config& config);
        ~Listener() override = default;

        /// Creates a "keyspace" string from a database name and collection spec.
        /// This is the database name, scope and collection name separated by ".".
        /// If the scope is default, it's omitted.
        /// If the scope and collection name are default, the keyspace is just the database name.
        static std::string makeKeyspace(std::string_view dbName, C4CollectionSpec const&);

        /// Takes apart a "keyspace" string into a database name and collection spec.
        static std::pair<std::string_view, C4CollectionSpec> parseKeyspace(fleece::slice ks LIFETIMEBOUND);

        /** Determines whether a database name is valid for use as a URI path component.
            It must be nonempty, no more than 240 bytes long, not start with an underscore,
            and contain no control characters. */
        static bool isValidDatabaseName(const std::string&);

        /// Given a filesystem path to a database, returns the database name.
        /// (This takes the last path component and removes the ".cblite2" extension.)
        /// Returns an empty string if the path is not a database, or if the name would not
        /// be valid according to isValidDatabaseName().
        static std::string databaseNameFromPath(const FilePath&);

        /// Makes a database visible via the REST API.
        /// By default, only its default collection is served. Call `registerCollection` to add others.
        /// @param db  The database to share. On success this instance is now managed by the Listener
        ///     and should not be used again by the caller.
        /// @param name  The URI name (first path component) in the HTTP API.
        ///     If not given, the C4Database's name will be used (possibly URL-escaped).
        /// @param dbConfig  Optional configuration for this database. Oerrides the C4ListenerConfig.
        /// @returns  True on success, false if the name is already in use.
        bool registerDatabase(C4Database* NONNULL db, std::optional<std::string> name = std::nullopt,
                              C4ListenerDatabaseConfig const* dbConfig = nullptr);

        /// Unregisters a database by its registered URI name.
        bool unregisterDatabase(const std::string& name);

        /// Unregisters a database. `db` need not be the exact instance that was registered;
        /// any instance on the same database file will work.
        bool unregisterDatabase(C4Database* db);

        /// Adds a collection to be shared.
        /// @note  A database's default collection is automatically shared.
        /// @param name  The URI name the database is registered by.
        /// @param collection  The C4CollectionSpec identifying the collection.
        /// @returns  True on success, false if the name is not registered. */
        bool registerCollection(const std::string& name, CollectionSpec collection);

        /// Unregisters a collection.
        /// @note  You can use this after `registerDatabase` to unregister the default collection.
        /// @param name  The URI name the database is registered by.
        /// @param collection  The C4CollectionSpec identifying the collection.
        /// @returns  True on success, false if the database name or collection is not registered. */
        bool unregisterCollection(const std::string& name, CollectionSpec collection);

        /// Returns the name a database is registered under.
        /// `db` need not be the exact instance that was registered;
        /// any instance on the same database file will work. */
        std::optional<std::string> nameOfDatabase(C4Database* NONNULL) const;

        /// Returns all registered database names.
        std::vector<std::string> databaseNames() const;

        /** Struct representing a shared database. */
        struct DBShare {
            fleece::Retained<DatabasePool> pool;       /// Pool of C4Database instances
            std::set<std::string>          keySpaces;  /// Shared collections
            DatabaseConfig                 config;     /// Configuration
        };

        /// Returns a copy of the sharing info for a database.
        std::optional<DBShare> getShare(std::string const& name) const;

        /// Returns a temporary C4Database instance, by the shared name.
        BorrowedDatabase borrowDatabaseNamed(const std::string& name, bool writeable) const;

        /// Returns a temporary C4Collection instance, by the shared db name and keyspace.
        BorrowedCollection borrowCollection(const std::string& keyspace, bool writeable) const;

        /// Returns the number of client connections.
        virtual int connectionCount() = 0;

        /// Returns the number of active client connections (for some definition of "active").
        virtual int activeConnectionCount() = 0;

        void closeDatabases();

      protected:
        mutable std::mutex _mutex;
        Config             _config;

      private:
        DBShare*       _getShare(std::string const& name);
        DBShare const* _getShare(std::string const& name) const;

        std::map<std::string, DBShare> _databases;
    };

}  // namespace litecore::REST

#endif
