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

#ifdef __cplusplus
extern "C" {
#endif

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
        kC4PrivateKeyData,              ///< PEM or DER data (may be PKCS12-encrypted)
    };


    /** TLS configuration for C4Listener. */
    typedef struct C4TLSConfig {
        C4PrivateKeyRepresentation privateKeyRepresentation; ///< Interpretation of `privateKey`
        C4Slice privateKey;             ///< Private key data
        C4String privateKeyPassword;    ///< Password to decrypt private key data
        C4Slice certificate;            ///< X.509 certificate data
        bool requireClientCerts;        ///< True to require clients to authenticate with a cert
        C4Slice rootClientCerts;        ///< Root CA certs to trust when verifying client cert
    } C4TLSConfig;


    /** Configuration for a C4Listener. */
    typedef struct C4ListenerConfig {
        uint16_t port;                  ///< TCP port to listen on
        C4String networkInterface;      ///< name or address of interface to listen on; else all
        C4ListenerAPIs apis;            ///< Which API(s) to enable
        C4TLSConfig* tlsConfig;         ///< TLS configuration, or NULL for no TLS

        // For REST listeners only:
        C4String directory;             ///< Directory where newly-PUT databases will be created
        bool allowCreateDBs;            ///< If true, "PUT /db" is allowed
        bool allowDeleteDBs;            ///< If true, "DELETE /db" is allowed

        // For sync listeners only:
        bool allowPush;
        bool allowPull;
    } C4ListenerConfig;


    /** Returns flags for the available APIs in this build (REST, sync, or both.) */
    C4ListenerAPIs c4listener_availableAPIs(void) C4API;

    /** Starts a new listener. */
    C4Listener* c4listener_start(const C4ListenerConfig *config C4NONNULL, C4Error *error) C4API;

    /** Closes and disposes a listener. */
    void c4listener_free(C4Listener *listener) C4API;

    /** Makes a database available from the network.
        @param listener  The listener that should share the database.
        @param name  The URI name to share it under, i.e. the path component in the URL.
                      If this is left null, a name will be chosen based as though you had called
                      \ref c4db_URINameFromPath.
        @param db  The database to share.
        @return  True on success, false if the name is invalid as a URI component. */
    bool c4listener_shareDB(C4Listener *listener C4NONNULL,
                            C4String name,
                            C4Database *db C4NONNULL,
                            C4Error *outError) C4API;

    /** Makes a previously-shared database unavailable. */
    bool c4listener_unshareDB(C4Listener *listener C4NONNULL,
                              C4Database *db C4NONNULL,
                              C4Error *outError) C4API;

    /** Returns the URL(s) of a database being shared, or of the root, separated by "\n" bytes.
        The URLs will differ only in their hostname -- there will be one for each IP address or known
        hostname of the computer, or of the network interface.
        @param listener  The active listener.
        @param db  A database being shared, or NULL to get the listener's root URL(s).
        @return  One or more URLs separated by "\n", or a null slice on error (unlikely).
                Caller is responsible for releasing the result. */
    C4StringResult c4listener_getURLs(C4Listener *listener C4NONNULL,
                                      C4Database *db) C4API;

    /** Returns the port number the listener is accepting connections on.
        This is useful if you didn't specify a port in the config (`port`=0), so you can find out which
        port the kernel picked. */
    uint16_t c4listener_getPort(C4Listener *listener C4NONNULL) C4API;

    /** A convenience that, given a filesystem path to a database, returns the database name
        for use in an HTTP URI path.
         - The directory portion of the path and the ".cblite2" extension are removed.
         - Any leading "_" is replaced with a "-".
         - Any control characters or slashes are replaced with "-".
        @param path  The filesystem path of a database.
        @return  A name that can be used as a URI path component, or NULL if the path is not a valid
                database path (does not end with ".cblite2".) */
    C4StringResult c4db_URINameFromPath(C4String path) C4API;

#ifdef __cplusplus
}
#endif
