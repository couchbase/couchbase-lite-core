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
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Listener  Network Listener: REST API and Sync Server
        @{ */

    /** Returns flags for the available APIs in this build (REST, sync, or both.) */
    CBL_CORE_API C4ListenerAPIs c4listener_availableAPIs(void) C4API;

    /** Starts a new listener. */
    CBL_CORE_API C4Listener* c4listener_start(const C4ListenerConfig *config,
                                 C4Error* C4NULLABLE error) C4API;

    /** Closes and disposes a listener. */
    CBL_CORE_API void c4listener_free(C4Listener * C4NULLABLE listener) C4API;

    /** Makes a database available from the network.
        @param listener  The listener that should share the database.
        @param name  The URI name to share it under, i.e. the path component in the URL.
                      If this is left null, a name will be chosen based as though you had called
                      \ref c4db_URINameFromPath.
        @param db  The database to share.
        @param outError On failure, the error info is stored here if non-NULL.
        @return  True on success, false if the name is invalid as a URI component. */
    CBL_CORE_API bool c4listener_shareDB(C4Listener *listener,
                            C4String name,
                            C4Database *db,
                            C4Error* C4NULLABLE outError) C4API;

    /** Makes a previously-shared database unavailable. */
    CBL_CORE_API bool c4listener_unshareDB(C4Listener *listener,
                              C4Database *db,
                              C4Error* C4NULLABLE outError) C4API;

    /** Returns the URL(s) of a database being shared, or of the root, separated by "\n" bytes.
        The URLs will differ only in their hostname -- there will be one for each IP address or known
        hostname of the computer, or of the network interface.
     
        WARNING: Link-local IPv6 addresses are included in this list.  However, due to IPv6 specification
        rules, a scope ID is also required to connect to these addresses.  So if the address starts with fe80::
        you will need to take care on the other side to also incorporate the scope of of the client network interface
        into the URL when connecting (in short, it's probably best to avoid these but they are there if
        you would like to try)
        @param listener  The active listener.
        @param db  A database being shared, or NULL to get the listener's root URL(s).
        @param api The API variant for which the URLs should be retrieved.  If the listener is not running in the given mode,
                   or more than one mode is given, an error is returned
        @param err The error information, if any
        @return  Fleece array of or more URL strings, or null if an error occurred.
                Caller is responsible for releasing the result. */
    CBL_CORE_API FLMutableArray c4listener_getURLs(const C4Listener *listener,
                                      C4Database* C4NULLABLE db,
                                      C4ListenerAPIs api,
                                      C4Error* C4NULLABLE err) C4API;

    /** Returns the port number the listener is accepting connections on.
        This is useful if you didn't specify a port in the config (`port`=0), so you can find out which
        port the kernel picked. */
    CBL_CORE_API uint16_t c4listener_getPort(const C4Listener *listener) C4API;

    /** Returns the number of client connections, and how many of those are currently active.
        Either parameter can be NULL if you don't care about it. */
    CBL_CORE_API void c4listener_getConnectionStatus(const C4Listener *listener,
                                        unsigned * C4NULLABLE connectionCount,
                                        unsigned * C4NULLABLE activeConnectionCount) C4API;

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
