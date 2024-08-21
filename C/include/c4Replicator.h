//
// c4Replicator.h
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
#include "c4ReplicatorTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup Replicator Replicator
        @{ */


/** Checks whether a database name is valid, for purposes of appearing in a replication URL */
CBL_CORE_API bool c4repl_isValidDatabaseName(C4String dbName) C4API;

/** Checks whether the destination of a replication is valid.
        (c4repl_new makes the same checks; this function is exposed so CBL can fail sooner.) */
NODISCARD CBL_CORE_API bool c4repl_isValidRemote(C4Address remoteAddress, C4String remoteDatabaseName,
                                                 C4Error* C4NULLABLE outError) C4API;

/** A simple URL parser that populates a C4Address from a URL string.
        The fields of the address will point inside the url string.
        @param url  The URL to be parsed.
        @param address  On sucess, the fields of the struct this points to will be populated with
                        the address components. This that are slices will point into the
                        appropriate substrings of `url`.
        @param dbName  If non-NULL, then on success this slice will point to the last path
                        component of `url`; `address->path` will not include this component.
        @return  True on success, false on failure. */
NODISCARD CBL_CORE_API bool c4address_fromURL(C4String url, C4Address* address, C4String* C4NULLABLE dbName) C4API;

/** Converts a C4Address to a URL. */
CBL_CORE_API C4StringResult c4address_toURL(C4Address address) C4API;


/** Creates a new networked replicator.
        \note The caller must use a lock for Database when this function is called.
        @param db  The local database.
        @param remoteAddress  The address of the remote server.
        @param remoteDatabaseName  The name of the database at the remote address.
        @param params Replication parameters (see above.)
        @param logPrefix prefix to the loggingClassName of returned C4Replicator.
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
NODISCARD CBL_CORE_API C4Replicator* c4repl_new(C4Database* db, C4Address remoteAddress, C4String remoteDatabaseName,
                                                C4ReplicatorParameters params, C4String logPrefix,
                                                C4Error* C4NULLABLE outError) C4API;

#ifdef COUCHBASE_ENTERPRISE
/** Creates a new replicator to another local database.
        \note The caller must use a lock for Database when this function is called.
        @param db  The local database.
        @param otherLocalDB  The other local database.
        @param params Replication parameters (see above.)
        @param logPrefix prefix to the loggingClassName of returned C4Replicator.
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
NODISCARD CBL_CORE_API C4Replicator* c4repl_newLocal(C4Database* db, C4Database* otherLocalDB,
                                                     C4ReplicatorParameters params, C4String logPrefix,
                                                     C4Error* C4NULLABLE outError) C4API;
#endif

/** Creates a new replicator from an already-open C4Socket. This is for use by listeners
        that accept incoming connections, wrap them by calling `c4socket_fromNative()`, then
        start a passive replication to service them.
        \note The caller must use a lock for Database when this function is called.
        @param db  The local database.
        @param openSocket  An already-created C4Socket.
        @param params  Replication parameters. Will usually use kC4Passive modes.
        @param logPrefix prefix to the loggingClassName of returned C4Replicator.
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
NODISCARD CBL_CORE_API C4Replicator* c4repl_newWithSocket(C4Database* db, C4Socket* openSocket,
                                                          C4ReplicatorParameters params, C4String logPrefix,
                                                          C4Error* C4NULLABLE outError) C4API;

/** Tells a replicator to start. Ignored if it's not in the Stopped state.
        \note This function is thread-safe.
        \note Do not call this function while a transaction is open on the same database as it can deadlock when \p repl tries to acquire the database.

        @param repl  The C4Replicator instance.
        @param reset If true, the replicator will reset its checkpoint and start replication from the beginning
     */
CBL_CORE_API void c4repl_start(C4Replicator* repl, bool reset) C4API;

/** Tells a replicator to stop. Ignored if in the Stopped state.
        \note This function is thread-safe. */
CBL_CORE_API void c4repl_stop(C4Replicator* repl) C4API;

/** Tells a replicator that's in the offline state to reconnect immediately.
        \note This function is thread-safe.
        @param repl  The replicator.
        @param outError  On failure, error information is stored here.
        @return  True if the replicator will reconnect, false if it won't. */
NODISCARD CBL_CORE_API bool c4repl_retry(C4Replicator* repl, C4Error* C4NULLABLE outError) C4API;

/** Informs the replicator whether it's considered possible to reach the remote host with
        the current network configuration. The default value is true. This only affects the
        replicator's behavior while it's in the Offline state:
        * Setting it to false will cancel any pending retry and prevent future automatic retries.
        * Setting it back to true will initiate an immediate retry.
        \note This function is thread-safe. */
CBL_CORE_API void c4repl_setHostReachable(C4Replicator* repl, bool reachable) C4API;

/** Puts the replicator in or out of "suspended" state.
        * Setting suspended=true causes the replicator to disconnect and enter Offline state;
          it will not attempt to reconnect while it's suspended.
        * Setting suspended=false causes the replicator to attempt to reconnect, _if_ it was
          connected when suspended, and is still in Offline state.
        \note This function is thread-safe. */
CBL_CORE_API void c4repl_setSuspended(C4Replicator* repl, bool suspended) C4API;

/** Sets the replicator's options dictionary.
        The changes will take effect next time the replicator connects.
        \note This function is thread-safe.  */
CBL_CORE_API void c4repl_setOptions(C4Replicator* repl, C4Slice optionsDictFleece) C4API;

/** Returns the current state of a replicator.
        \note This function is thread-safe.  */
CBL_CORE_API C4ReplicatorStatus c4repl_getStatus(C4Replicator* repl) C4API;

/** Returns the HTTP response headers as a Fleece-encoded dictionary.
        \note This function is thread-safe.  */
CBL_CORE_API C4Slice c4repl_getResponseHeaders(C4Replicator* repl) C4API;

/** Gets a fleece encoded list of IDs of documents who have revisions pending push.  This
     *  API is a snapshot and results may change between the time the call was made and the time
     *  the call returns.
     *
     *  \note This function is thread-safe.
     *  @param repl  The C4Replicator instance.
     *  @param spec  The collection spec
     *  @param outErr On failure, error information will be written here if non-NULL.
     *  @return A fleece encoded array of document IDs, each of which has one or more pending
     *  revisions.  If none are pending, nullslice is returned (note that an error
     * condition will return nullslice with the outErr code set to non-zero)
     */
CBL_CORE_API C4SliceResult c4repl_getPendingDocIDs(C4Replicator* repl, C4CollectionSpec spec,
                                                   C4Error* C4NULLABLE outErr) C4API;

/** Checks if the document with the given ID has revisions pending push.  This
     *  API is a snapshot and results may change between the time the call was made and the time
     *  the call returns.
     *
     *  \note This function is thread-safe.
     *  @param repl  The C4Replicator instance.
     *  @param docID The ID of the document to check
     *  @param spec The collection the docID belongs to.
     *  @param outErr On failure, error information will be written here if non-NULL.
     *  @return true if the document has one or more revisions pending, false otherwise (note that an error
     *  condition will return false with the outErr code set to non-zero)
     */
NODISCARD CBL_CORE_API bool c4repl_isDocumentPending(C4Replicator* repl, C4String docID, C4CollectionSpec spec,
                                                     C4Error* C4NULLABLE outErr) C4API;


/** Gets the TLS certificate, if any, that was sent from the remote server (NOTE: Only functions when using BuiltInWebSocket) 
    \note This function is thread-safe. */
NODISCARD CBL_CORE_API C4Cert* c4repl_getPeerTLSCertificate(C4Replicator* repl, C4Error* C4NULLABLE outErr) C4API;

/** Sets the progress level of the replicator, indicating what information should be provided via
     *  callback.
     *
     * \note The caller must use a lock for Replicator when this function is called.
     *  @param repl  The C4Replicator instance.
     *  @param level The progress level to set on the replicator
     *  @param outErr Records error information, if any.
     *  @return true if the progress level was set, false if there was an error.
     */
NODISCARD CBL_CORE_API bool c4repl_setProgressLevel(C4Replicator* repl, C4ReplicatorProgressLevel level,
                                                    C4Error* C4NULLABLE outErr) C4API;


#pragma mark - COOKIES:


/** Takes the value of a "Set-Cookie:" header, received from the given host, from an HTTP
     *  request with the given path, and saves the cookie into the database's cookie store.
     *  (Persistent cookies are saved as metadata in the database file until they expire.
     *  Session cookies are kept in memory, until the last C4Database handle to the given database
     *  is closed.)
     *
     *  \note The caller must use a lock for Database when this function is called.
     *  @param db  The C4Databaser instance.
     *  @param setCookieHeader The "Set-Cookie" header.
     *  @param fromHost The host address of the request.
     *  @param fromPath The path of the request.
     *  @param acceptParentDomain Whether to allow the "Domain" property of the cookie to be a parent domain of the host address. It should match the option, kC4ReplicatorOptionAcceptParentDomainCookies, in C4ReplicatorParameters.
     *  @param outError Records error information, if any.
     *  @return true if the cookie is successfully saved..
     */
NODISCARD CBL_CORE_API bool c4db_setCookie(C4Database* db, C4String setCookieHeader, C4String fromHost,
                                           C4String fromPath, bool acceptParentDomain,
                                           C4Error* C4NULLABLE outError) C4API;

/** Locates any saved HTTP cookies relevant to the given request, and returns them as a string
        that can be used as the value of a "Cookie:" header. 
        \note The caller must use a lock for Database when this function is called. */
CBL_CORE_API C4StringResult c4db_getCookies(C4Database* db, C4Address request, C4Error* C4NULLABLE error) C4API;

/** Removes all cookies from the database's cookie store. 
        \note The caller must use a lock for Database when this function is called. */
CBL_CORE_API void c4db_clearCookies(C4Database* db) C4API;

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
