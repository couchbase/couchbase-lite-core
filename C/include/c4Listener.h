//
// c4Listener.h
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
#include "c4Base.h"
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Listener  Network Listener: REST API and Sync Server
        @{ */


    /** Flags indicating which network API(s) to serve. */
    typedef C4_OPTIONS(unsigned, C4ListenerAPIs) {
        kC4RESTAPI = 0x01,              ///< CouchDB-like REST API
        kC4SyncAPI = 0x02               ///< Replication server
    };


    /** Different ways to provide TLS private keys. */
    typedef C4_ENUM(unsigned, C4PrivateKeyRepresentation) {
        kC4PrivateKeyFromCert,          ///< Key in secure storage, associated with certificate
        kC4PrivateKeyFromKey,           ///< Key from the provided key pair
    };


    /** Called when a client connects, during the TLS handshake, if a client certificate is received.
        @param listener  The C4Listener handling the connection.
        @param clientCertData  The client's X.509 certificate in DER encoding.
        @param context  The `tlsCallbackContext` from the `C4TLSConfig`.
        @return  True to allow the connection, false to refuse it. */
    typedef bool (*C4ListenerCertAuthCallback)(C4Listener *listener,
                                               C4Slice clientCertData,
                                               void * C4NULLABLE context);

    /** Called when a client connects, after the TLS handshake (if any), when the initial HTTP request is
        received.
        @param listener  The C4Listener handling the connection.
        @param authHeader  The "Authorization" header value from the client's HTTP request, or null.
        @param context  The `callbackContext` from the listener config.
        @return  True to allow the connection, false to refuse it. */
    typedef bool (*C4ListenerHTTPAuthCallback)(C4Listener *listener,
                                               C4Slice authHeader,
                                               void * C4NULLABLE context);

    /** TLS configuration for C4Listener. */
    typedef struct C4TLSConfig {
        C4PrivateKeyRepresentation privateKeyRepresentation; ///< Interpretation of `privateKey`
        C4KeyPair* key;                         ///< A key pair that contains the private key
        C4Cert* certificate;                    ///< X.509 certificate data
        bool requireClientCerts;                ///< True to require clients to authenticate with a cert
        C4Cert* C4NULLABLE rootClientCerts;     ///< Root CA certs to trust when verifying client cert
        C4ListenerCertAuthCallback C4NULLABLE certAuthCallback; ///< Callback for X.509 cert auth
        void* C4NULLABLE tlsCallbackContext;
    } C4TLSConfig;


    /** Configuration for a C4Listener. */
    typedef struct C4ListenerConfig {
        uint16_t port;                          ///< TCP port to listen on
        C4String networkInterface;              ///< name or address of interface to listen on; else all
        C4ListenerAPIs apis;                    ///< Which API(s) to enable
        C4TLSConfig* C4NULLABLE tlsConfig;      ///< TLS configuration, or NULL for no TLS

        C4ListenerHTTPAuthCallback C4NULLABLE httpAuthCallback; ///< Callback for HTTP auth
        void* C4NULLABLE callbackContext;       ///< Client value passed to HTTP auth callback

        // For REST listeners only:
        C4String directory;                     ///< Directory where newly-PUT databases will be created
        bool allowCreateDBs;                    ///< If true, "PUT /db" is allowed
        bool allowDeleteDBs;                    ///< If true, "DELETE /db" is allowed

        // For sync listeners only:
        bool allowPush;                         ///< Allow peers to push changes to local db
        bool allowPull;                         ///< Allow peers to pull changes from local db
        bool enableDeltaSync;                   ///< Enable document-deltas optimization
    } C4ListenerConfig;


    /** Returns flags for the available APIs in this build (REST, sync, or both.) */
    C4ListenerAPIs c4listener_availableAPIs(void) C4API;

    /** Starts a new listener. */
    C4Listener* c4listener_start(const C4ListenerConfig *config,
                                 C4Error* C4NULLABLE error) C4API;

    /** Closes and disposes a listener. */
    void c4listener_free(C4Listener * C4NULLABLE listener) C4API;

    /** Makes a database available from the network.
        @param listener  The listener that should share the database.
        @param name  The URI name to share it under, i.e. the path component in the URL.
                      If this is left null, a name will be chosen based as though you had called
                      \ref c4db_URINameFromPath.
        @param db  The database to share.
        @param outError On failure, the error info is stored here if non-NULL.
        @return  True on success, false if the name is invalid as a URI component. */
    bool c4listener_shareDB(C4Listener *listener,
                            C4String name,
                            C4Database *db,
                            C4Error* C4NULLABLE outError) C4API;

    /** Makes a previously-shared database unavailable. */
    bool c4listener_unshareDB(C4Listener *listener,
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
    FLMutableArray c4listener_getURLs(C4Listener *listener,
                                      C4Database* C4NULLABLE db,
                                      C4ListenerAPIs api,
                                      C4Error* C4NULLABLE err) C4API;

    /** Returns the port number the listener is accepting connections on.
        This is useful if you didn't specify a port in the config (`port`=0), so you can find out which
        port the kernel picked. */
    uint16_t c4listener_getPort(C4Listener *listener) C4API;

    /** Returns the number of client connections, and how many of those are currently active.
        Either parameter can be NULL if you don't care about it. */
    void c4listener_getConnectionStatus(C4Listener *listener,
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
    C4StringResult c4db_URINameFromPath(C4String path) C4API;


/** @} */
    
C4API_END_DECLS
C4_ASSUME_NONNULL_END
