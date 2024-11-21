//
// c4Listener.h
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
#include "c4ListenerTypes.h"
#include "fleece/FLBase.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup Listener  Network Listener: REST API and Sync Server
        @{ */

/** Creates and starts a new listener. Caller must release it when done. */
NODISCARD CBL_CORE_API C4Listener* C4NULLABLE c4listener_start(const C4ListenerConfig* config,
                                                               C4Error* C4NULLABLE     error) C4API;

/** Makes a database available from the network, and its default collection.
    This function is equivalent to `c4listener_shareDBWithConfig`, with `config` being `NULL`. */
NODISCARD CBL_CORE_API bool c4listener_shareDB(C4Listener* listener, C4String name, C4Database* db,
                                               C4Error* C4NULLABLE outError) C4API;

/** Makes a database available from the network, and its default collection.
    @note The caller must use a lock for the C4Database when this function is called.
    @param listener  The listener that should share the database.
    @param name  The URI name to share it under, i.e. the path component in the URL.
                 If this is left null, a name will be chosen by calling \ref c4db_URINameFromPath.
                 The name may not include '/', '.', control characters, or non-ASCII characters.
    @param db  The database to share.
    @param config  Per-database configuration overriding the `C4ListenerConfig`, or `NULL`.
    @param outError On failure, the error info is stored here if non-NULL.
    @return  True on success, false if the name is already in use or invalid as a URI component. */
NODISCARD CBL_CORE_API bool c4listener_shareDBWithConfig(C4Listener* listener, C4String name, C4Database* db,
                                                         const C4ListenerDatabaseConfig* config,
                                                         C4Error* C4NULLABLE             outError) C4API;

/** Makes a previously-shared database unavailable.
    @note  `db` need not be the same instance that was registered, merely on the same file.
    @note The caller must use a lock for the C4Database when this function is called. */
NODISCARD CBL_CORE_API bool c4listener_unshareDB(C4Listener* listener, C4Database* db,
                                                 C4Error* C4NULLABLE outError) C4API;

/** Specifies a collection to be used in a P2P listener context.  NOTE: A database
    must have been previously shared under the same name, or this operation will fail.
    @note The caller must use a lock for the C4Collection when this function is called.
    @param listener  The listener that should share the collection.
    @param name  The URI name to share it under, this must match the name of an already
                 shared DB.
    @param collection  The collection to share.
    @param outError On failure, the error info is stored here if non-NULL. */
NODISCARD CBL_CORE_API bool c4listener_shareCollection(C4Listener* listener, C4String name, C4Collection* collection,
                                                       C4Error* C4NULLABLE outError) C4API;

/** Makes a previously-shared collection unavailable.
    @note The caller must use a lock for the C4Collection when this function is called. */
NODISCARD CBL_CORE_API bool c4listener_unshareCollection(C4Listener* listener, C4String name, C4Collection* collection,
                                                         C4Error* C4NULLABLE outError) C4API;

/** Returns the URL(s) of a database being shared, or of the root.
    The URLs will differ only in their hostname -- there will be one for each IP address or known
    hostname of the computer, or of the network interface.

    WARNING: Link-local IPv6 addresses are included in this list.  However, due to IPv6 specification
    rules, a scope ID is also required to connect to these addresses.  So if the address starts with fe80::
    you will need to take care on the other side to also incorporate the scope of of the client network interface
    into the URL when connecting (in short, it's probably best to avoid these but they are there if
    you would like to try)
    @note The caller must use a lock for the C4Database when this function is called.
    @note The caller is responsible for releasing the returned Fleece array.
    @param listener  The active listener.
    @param db  A database being shared, or NULL to get the listener's root URL(s).
    @param err The error information, if any
    @return  Fleece array of or more URL strings, or null if an error occurred.
            Caller is responsible for releasing the result. */
NODISCARD CBL_CORE_API FLMutableArray c4listener_getURLs(const C4Listener* listener, C4Database* C4NULLABLE db,
                                                         C4Error* C4NULLABLE err) C4API;

/** Returns the port number the listener is accepting connections on.
    This is useful if you didn't specify a port in the config (`port`=0), so you can find out which
    port the kernel picked.
    \note This function is thread-safe. */
CBL_CORE_API uint16_t c4listener_getPort(const C4Listener* listener) C4API;

/** Returns the number of client connections, and how many of those are currently active.
    Either parameter can be NULL if you don't care about it.
    \note This function is thread-safe. */
CBL_CORE_API void c4listener_getConnectionStatus(const C4Listener* listener, unsigned* C4NULLABLE connectionCount,
                                                 unsigned* C4NULLABLE activeConnectionCount) C4API;

/** A convenience that, given a filesystem path to a database, returns the database name
    for use in an HTTP URI path.
     - The directory portion of the path and the ".cblite2" extension are removed.
     - Any leading "_" is replaced with a "-".
     - Any control characters or slashes are replaced with "-".
    @param path  The filesystem path of a database.
    @return  A name that can be used as a URI path component, or NULL if the path is not a valid
            database path (does not end with ".cblite2".) */
CBL_CORE_API C4StringResult c4db_URINameFromPath(C4String path) C4API;


/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
