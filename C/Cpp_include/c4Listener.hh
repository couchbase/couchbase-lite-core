//
// c4Listener.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4ListenerTypes.h"
#include "fleece/FLBase.h"
#include "fleece/InstanceCounted.hh"
#include <vector>

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** A lightweight server that shares databases over the network for replication. */
struct C4Listener final
    : public fleece::InstanceCounted
    , C4Base {
    /// Constructor. Starts the listener (asynchronously) but does not share any databases.
    explicit C4Listener(C4ListenerConfig const& config);

    ~C4Listener() override;

    /// Shares a database, and its default collection.
    /// @param name  The URI name (first path component) in the HTTP API.
    ///     If `nullslice`, the C4Database's name will be used (possibly URL-escaped).
    ///     The name may not include '/', '.', control characters, or non-ASCII characters.
    /// @param db  The database to share. On success this instance is now managed by the Listener
    ///     and should not be used again by the caller.
    /// @param dbConfig  Optional configuration for this database. Overrides the C4ListenerConfig.
    /// @returns  True on success, false if the name is already in use.
    [[nodiscard]] bool shareDB(slice name, C4Database* db, C4ListenerDatabaseConfig const* dbConfig = nullptr);

    /// Stops sharing a database. `db` need not be the exact instance that was registered;
    /// any instance on the same database file will work.
    bool unshareDB(C4Database* db);

    /// Adds a collection to be shared.
    /// @note  A database's default collection is automatically shared.
    /// @param name  The URI name the database is registered by.
    /// @param collection  The collection instance to share.
    /// @returns  True on success, false if `name` is not registered. */
    [[nodiscard]] bool shareCollection(slice name, C4Collection* collection);

    /// Stops sharing a collection.
    /// @note  Call this after \ref registerDatabase if you don't want to share the default collection.
    /// @param name  The URI name the database is registered by.
    /// @param collection  The collection instance.
    /// @returns  True on success, false if the database name or collection is not registered. */
    bool unshareCollection(slice name, C4Collection* collection);

    /// The TCP port number for incoming connections.
    [[nodiscard]] uint16_t port() const;

    /// Returns first the number of connections, and second the number of active connections.
    [[nodiscard]] std::pair<unsigned, unsigned> connectionStatus() const;

    /// Returns the URL(s) of a database being shared, or of the root.
    /// The URLs will differ only in their hostname -- there will be one for each IP address or known
    /// hostname of the computer, or of the network interface.
    [[nodiscard]] std::vector<std::string> URLs(C4Database* C4NULLABLE db) const;

    /// A convenience that, given a filesystem path to a database, returns the database name
    /// for use in an HTTP URI path.
    [[nodiscard]] static std::string URLNameFromPath(slice path);

    C4Listener(const C4Listener&) = delete;

    // internal use only
    C4Listener(C4ListenerConfig const& config, Retained<litecore::REST::HTTPListener> impl);

  private:
    C4Listener(C4Listener&&) noexcept;

    Retained<litecore::REST::HTTPListener> _impl;
};

C4_ASSUME_NONNULL_END
