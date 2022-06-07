//
// c4ListenerTypes.h
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

    bool allowConnectedClient;              ///< Allow peers to use Connected Client API
    FLDict namedQueries;                    ///< Maps query names to N1QL or JSON source
    bool allowArbitraryQueries;             ///< If true, client can run arbitrary queries
} C4ListenerConfig;


/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
