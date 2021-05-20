//
// c4Replicator.h
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
#include "c4Document.h"
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Replicator Replicator
        @{ */

#define kC4Replicator2Scheme    C4STR("ws")
#define kC4Replicator2TLSScheme C4STR("wss")

    /** How to replicate, in either direction */
    typedef C4_ENUM(int32_t, C4ReplicatorMode) {
        kC4Disabled,        // Do not allow this direction
        kC4Passive,         // Allow peer to initiate this direction
        kC4OneShot,         // Replicate, then stop
        kC4Continuous       // Keep replication active until stopped by application
    };

    /** The possible states of a replicator. */
    typedef C4_ENUM(int32_t, C4ReplicatorActivityLevel) {
        /* EXTERNAL STATES */
        kC4Stopped,     ///< Finished, or got a fatal error.
        kC4Offline,     ///< Connection failed, but waiting to retry. */
        kC4Connecting,  ///< Connection is in progress.
        kC4Idle,        ///< Continuous replicator has caught up and is waiting for changes.
        kC4Busy,         ///< Connected and actively working.
        
        /* INTERNAL STATES */
        kC4Stopping,    ///< Stopping or going offline
    };

    /** For convenience, an array of C strings naming the C4ReplicatorActivityLevel values. */
    CBL_CORE_API extern const char* C4NONNULL const kC4ReplicatorActivityLevelNames[6];


    /** A simple parsed-URL type. */
    struct C4Address {
        C4String scheme;
        C4String hostname;
        uint16_t port;
        C4String path;
    };


    /** Represents the current progress of a replicator.
        The `units` fields should not be used directly, but divided (`unitsCompleted`/`unitsTotal`)
        to give a _very_ approximate progress fraction. */
    typedef struct {
        uint64_t    unitsCompleted;     ///< Abstract number of work units completed so far
        uint64_t    unitsTotal;         ///< Total number of work units (a very rough approximation)
        uint64_t    documentCount;      ///< Number of documents transferred so far
    } C4Progress;

    /** Flags relating to a replicator's connection state. */
    typedef C4_OPTIONS(int32_t, C4ReplicatorStatusFlags) {
        kC4WillRetry     = 0x1,         ///< If true, will automatically reconnect when offline
        kC4HostReachable = 0x2,         ///< If false, it's not possible to connect to the host
        kC4Suspended     = 0x4          ///< If true, will not connect until unsuspended
    };

    /** An enumeration of the levels of progress callbacks the replicator can provide.
     *  Each level is serviced by a different callback.  The higher the level, the more
     *  notifications that the replicator has to send out, which has an impact on performance,
     *  since it takes up time in the execution queue.
     */
    typedef C4_ENUM(int32_t, C4ReplicatorProgressLevel) {
        kC4ReplProgressOverall,         ///< Callback about completion and estimated total (C4ReplicatorStatusChangedCallback)
        kC4ReplProgressPerDocument,     ///< Callback for every document replicated (C4ReplicatorDocumentsEndedCallback)
        kC4ReplProgressPerAttachment,   ///< Callback for every document and attachment replicated (C4ReplicatorBlobProgressCallback)
    };

    /** Current status of replication. Passed to `C4ReplicatorStatusChangedCallback`. */
    typedef struct {
        C4ReplicatorActivityLevel level;
        C4Progress progress;
        C4Error error;
        C4ReplicatorStatusFlags flags;
    } C4ReplicatorStatus;

    /** Information about a document that's been pushed or pulled. */
    typedef struct {
        C4HeapString docID;
        C4HeapString revID;
        C4RevisionFlags flags;
        C4SequenceNumber sequence;
        C4Error error;
        bool errorIsTransient;
    } C4DocumentEnded;


    /** Callback a client can register, to get progress information.
        This will be called on arbitrary background threads, and should not block. */
    typedef void (*C4ReplicatorStatusChangedCallback)(C4Replicator*,
                                                      C4ReplicatorStatus,
                                                      void * C4NULLABLE context);

    /** Callback a client can register, to hear about the replication status of documents.
        By default, only errors will be reported via this callback.
        To also receive callbacks for successfully completed documents, set the
        kC4ReplicatorOptionProgressLevel option to a value greater than zero. */
    typedef void (*C4ReplicatorDocumentsEndedCallback)(C4Replicator*,
                                                       bool pushing,
                                                       size_t numDocs,
                                                       const C4DocumentEnded* C4NONNULL docs[C4NONNULL],
                                                       void * C4NULLABLE context);

    /** Callback a client can register, to hear about the status of blobs. */
    typedef void (*C4ReplicatorBlobProgressCallback)(C4Replicator*,
                                                     bool pushing,
                                                     C4String docID,
                                                     C4String docProperty,
                                                     C4BlobKey blobKey,
                                                     uint64_t bytesComplete,
                                                     uint64_t bytesTotal,
                                                     C4Error error,
                                                     void * C4NULLABLE context);

    /** Callback that can choose to reject an incoming pulled revision, or stop a local
        revision from being pushed, by returning false.
        (Note: In the case of an incoming revision, no flags other than 'deletion' and
        'hasAttachments' will be set.) */
    typedef bool (*C4ReplicatorValidationFunction)(C4String docID,
                                                   C4String revID,
                                                   C4RevisionFlags,
                                                   FLDict body,
                                                   void* C4NULLABLE context);

    /** Checks whether a database name is valid, for purposes of appearing in a replication URL */
    bool c4repl_isValidDatabaseName(C4String dbName) C4API;

    /** Checks whether the destination of a replication is valid.
        (c4repl_new makes the same checks; this function is exposed so CBL can fail sooner.) */
    bool c4repl_isValidRemote(C4Address remoteAddress,
                              C4String remoteDatabaseName,
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
    bool c4address_fromURL(C4String url,
                           C4Address *address,
                           C4String * C4NULLABLE dbName) C4API;

    /** Converts a C4Address to a URL. */
    C4StringResult c4address_toURL(C4Address address) C4API;


    /** Parameters describing a replication, used when creating a C4Replicator. */
    typedef struct C4ReplicatorParameters {
        C4ReplicatorMode                    push;              ///< Push mode (from db to remote/other db)
        C4ReplicatorMode                    pull;              ///< Pull mode (from db to remote/other db).
        C4Slice                             optionsDictFleece; ///< Optional Fleece-encoded dictionary of optional parameters.
        C4ReplicatorValidationFunction C4NULLABLE      pushFilter;        ///< Callback that can reject outgoing revisions
        C4ReplicatorValidationFunction C4NULLABLE      validationFunc;    ///< Callback that can reject incoming revisions
        C4ReplicatorStatusChangedCallback C4NULLABLE   onStatusChanged;   ///< Callback to be invoked when replicator's status changes.
        C4ReplicatorDocumentsEndedCallback C4NULLABLE onDocumentsEnded;  ///< Callback notifying status of individual documents
        C4ReplicatorBlobProgressCallback C4NULLABLE    onBlobProgress;    ///< Callback notifying blob progress
        void* C4NULLABLE                               callbackContext;   ///< Value to be passed to the callbacks.
        const C4SocketFactory* C4NULLABLE              socketFactory;     ///< Custom C4SocketFactory, if not NULL
    } C4ReplicatorParameters;


    /** Creates a new networked replicator.
        @param db  The local database.
        @param remoteAddress  The address of the remote server.
        @param remoteDatabaseName  The name of the database at the remote address.
        @param params Replication parameters (see above.)
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
    C4Replicator* c4repl_new(C4Database* db,
                             C4Address remoteAddress,
                             C4String remoteDatabaseName,
                             C4ReplicatorParameters params,
                             C4Error* C4NULLABLE outError) C4API;

#ifdef COUCHBASE_ENTERPRISE
    /** Creates a new replicator to another local database.
        @param db  The local database.
        @param otherLocalDB  The other local database.
        @param params Replication parameters (see above.)
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
    C4Replicator* c4repl_newLocal(C4Database* db,
                                  C4Database* otherLocalDB,
                                  C4ReplicatorParameters params,
                                             C4Error* C4NULLABLE outError) C4API;
#endif

    /** Creates a new replicator from an already-open C4Socket. This is for use by listeners
        that accept incoming connections, wrap them by calling `c4socket_fromNative()`, then
        start a passive replication to service them.
        @param db  The local database.
        @param openSocket  An already-created C4Socket.
        @param params  Replication parameters. Will usually use kC4Passive modes.
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
    C4Replicator* c4repl_newWithSocket(C4Database* db,
                                       C4Socket *openSocket,
                                       C4ReplicatorParameters params,
                                       C4Error* C4NULLABLE outError) C4API;

    /** Frees a replicator reference.
        Does not stop the replicator -- if the replicator still has other internal references,
        it will keep going. If you need the replicator to stop, call \ref c4repl_stop first.
        \note This function is thread-safe. */
    void c4repl_free(C4Replicator* C4NULLABLE repl) C4API;

    /** Tells a replicator to start. Ignored if it's not in the Stopped state.
        \note This function is thread-safe.
        @param repl  The C4Replicator instance.
        @param reset If true, the replicator will reset its checkpoint and start replication from the beginning
     */
    void c4repl_start(C4Replicator* repl, bool reset) C4API;

    /** Tells a replicator to stop. Ignored if in the Stopped state.
        This function is thread-safe.  */
    void c4repl_stop(C4Replicator* repl) C4API;

    /** Tells a replicator that's in the offline state to reconnect immediately.
        \note This function is thread-safe.
        @param repl  The replicator.
        @param outError  On failure, error information is stored here.
        @return  True if the replicator will reconnect, false if it won't. */
    bool c4repl_retry(C4Replicator* repl, C4Error* C4NULLABLE outError) C4API;

    /** Informs the replicator whether it's considered possible to reach the remote host with
        the current network configuration. The default value is true. This only affects the
        replicator's behavior while it's in the Offline state:
        * Setting it to false will cancel any pending retry and prevent future automatic retries.
        * Setting it back to true will initiate an immediate retry.
        \note This function is thread-safe. */
    void c4repl_setHostReachable(C4Replicator* repl, bool reachable) C4API;

    /** Puts the replicator in or out of "suspended" state.
        * Setting suspended=true causes the replicator to disconnect and enter Offline state;
          it will not attempt to reconnect while it's suspended.
        * Setting suspended=false causes the replicator to attempt to reconnect, _if_ it was
          connected when suspended, and is still in Offline state.
        \note This function is thread-safe. */
    void c4repl_setSuspended(C4Replicator* repl, bool suspended) C4API;

    /** Sets the replicator's options dictionary.
        The changes will take effect next time the replicator connects.
        \note This function is thread-safe.  */
    void c4repl_setOptions(C4Replicator* repl, C4Slice optionsDictFleece) C4API;

    /** Returns the current state of a replicator.
        This function is thread-safe.  */
    C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl) C4API;

    /** Returns the HTTP response headers as a Fleece-encoded dictionary.
        \note This function is thread-safe.  */
    C4Slice c4repl_getResponseHeaders(C4Replicator *repl) C4API;

    /** Gets a fleece encoded list of IDs of documents who have revisions pending push.  This
     *  API is a snapshot and results may change between the time the call was made and the time
     *  the call returns.
     *
     *  @param repl  The C4Replicator instance.
     *  @param outErr On failure, error information will be written here if non-NULL.
     *  @return A fleece encoded array of document IDs, each of which has one or more pending
     *  revisions.  If none are pending, nullslice is returned (note that an error
     * condition will return nullslice with the outErr code set to non-zero)
     */
    C4SliceResult c4repl_getPendingDocIDs(C4Replicator* repl, C4Error* C4NULLABLE outErr) C4API;

    /** Checks if the document with the given ID has revisions pending push.  This
     *  API is a snapshot and results may change between the time the call was made and the time
     *  the call returns.
     *
     *  @param repl  The C4Replicator instance.
     *  @param docID The ID of the document to check
     *  @param outErr On failure, error information will be written here if non-NULL.
     *  @return true if the document has one or more revisions pending, false otherwise (note that an error
     *  condition will return false with the outErr code set to non-zero)
     */
    bool c4repl_isDocumentPending(C4Replicator* repl, C4String docID, C4Error* C4NULLABLE outErr) C4API;


    /** Gets the TLS certificate, if any, that was sent from the remote server (NOTE: Only functions when using BuiltInWebSocket) */
    C4Cert* c4repl_getPeerTLSCertificate(C4Replicator* repl,
                                         C4Error* C4NULLABLE outErr) C4API;

    /** Sets the progress level of the replicator, indicating what information should be provided via
     *  callback.
     *
     *  @param repl  The C4Replicator instance.
     *  @param level The progress level to set on the replicator
     *  @param outErr Records error information, if any.
     *  @return true if the progress level was set, false if there was an error.
     */
    bool c4repl_setProgressLevel(C4Replicator* repl,
                                 C4ReplicatorProgressLevel level,
                                 C4Error* C4NULLABLE outErr) C4API;


#pragma mark - COOKIES:


    /** Takes the value of a "Set-Cookie:" header, received from the given host, from an HTTP
        request with the given path, and saves the cookie into the database's cookie store.
        (Persistent cookies are saved as metadata in the database file until they expire.
        Session cookies are kept in memory, until the last C4Database handle to the given database
        is closed.) */
    bool c4db_setCookie(C4Database *db,
                        C4String setCookieHeader,
                        C4String fromHost,
                        C4String fromPath,
                        C4Error* C4NULLABLE outError) C4API;

    /** Locates any saved HTTP cookies relevant to the given request, and returns them as a string
        that can be used as the value of a "Cookie:" header. */
    C4StringResult c4db_getCookies(C4Database *db,
                                   C4Address request,
                                   C4Error* C4NULLABLE error) C4API;

    /** Removes all cookies from the database's cookie store. */
    void c4db_clearCookies(C4Database *db) C4API;


#pragma mark - CONSTANTS:


    // Replicator option dictionary keys:
    #define kC4ReplicatorOptionDocIDs           "docIDs"   ///< Docs to replicate (string[])
    #define kC4ReplicatorOptionChannels         "channels" ///< SG channel names (string[])
    #define kC4ReplicatorOptionFilter           "filter"   ///< Pull filter name (string)
    #define kC4ReplicatorOptionFilterParams     "filterParams"  ///< Pull filter params (Dict[string])
    #define kC4ReplicatorOptionSkipDeleted      "skipDeleted" ///< Don't push/pull tombstones (bool)
    #define kC4ReplicatorOptionNoIncomingConflicts "noIncomingConflicts" ///< Reject incoming conflicts (bool)
    #define kC4ReplicatorCheckpointInterval     "checkpointInterval" ///< How often to checkpoint, in seconds (number)
    #define kC4ReplicatorOptionRemoteDBUniqueID "remoteDBUniqueID" ///< Stable ID for remote db with unstable URL (string)
    #define kC4ReplicatorOptionProgressLevel    "progress"  ///< If >=1, notify on every doc; if >=2, on every attachment (int)
    #define kC4ReplicatorOptionDisableDeltas    "noDeltas"   ///< Disables delta sync (bool)
    #define kC4ReplicatorOptionMaxRetries       "maxRetries" ///< Max number of retry attempts (int)
    #define kC4ReplicatorOptionMaxRetryInterval "maxRetryInterval" ///< Max delay betw retries (secs)
    #define kC4ReplicatorOptionMaxAttempts      "maxAttempts" ///< Max number of attempts (int)
    #define kC4ReplicatorOptionMaxAttemptWaitTime "maxAttemptWaitTime" ///< Max delay betw attempts (secs)

    // TLS options:
    #define kC4ReplicatorOptionRootCerts        "rootCerts"  ///< Trusted root certs (data)
    #define kC4ReplicatorOptionPinnedServerCert "pinnedCert"  ///< Cert or public key (data)
    #define kC4ReplicatorOptionOnlySelfSignedServerCert "onlySelfSignedServer" ///< Only accept self signed server certs (for P2P, bool)

    // HTTP options:
    #define kC4ReplicatorOptionExtraHeaders     "headers"  ///< Extra HTTP headers (string[])
    #define kC4ReplicatorOptionCookies          "cookies"  ///< HTTP Cookie header value (string)
    #define kC4ReplicatorOptionAuthentication   "auth"     ///< Auth settings (Dict); see [1]
    #define kC4ReplicatorOptionProxyServer      "proxy"    ///< Proxy settings (Dict); see [3]]

    // WebSocket options:
    #define kC4ReplicatorHeartbeatInterval      "heartbeat" ///< Interval in secs to send a keepalive ping
    #define kC4SocketOptionWSProtocols          "WS-Protocols" ///< Sec-WebSocket-Protocol header value

    // BLIP options:
    #define kC4ReplicatorCompressionLevel       "BLIPCompressionLevel" ///< Data compression level, 0..9

    // [1]: Auth dictionary keys:
    #define kC4ReplicatorAuthType       "type"           ///< Auth type; see [2] (string)
    #define kC4ReplicatorAuthUserName   "username"       ///< User name for basic auth (string)
    #define kC4ReplicatorAuthPassword   "password"       ///< Password for basic auth (string)
    #define kC4ReplicatorAuthClientCert "clientCert"     ///< TLS client certificate (value platform-dependent)
    #define kC4ReplicatorAuthClientCertKey "clientCertKey" ///< Client cert's private key (data)
    #define kC4ReplicatorAuthToken      "token"          ///< Session cookie or auth token (string)

    // [2]: auth.type values:
    #define kC4AuthTypeBasic            "Basic"          ///< HTTP Basic (the default)
    #define kC4AuthTypeSession          "Session"        ///< SG session cookie
    #define kC4AuthTypeOpenIDConnect    "OpenID Connect" ///< OpenID Connect token
    #define kC4AuthTypeFacebook         "Facebook"       ///< Facebook auth token
    #define kC4AuthTypeClientCert       "Client Cert"    ///< TLS client cert

    // [3]: Proxy dictionary keys:
    #define kC4ReplicatorProxyType      "type"           ///< Proxy type; see [4] (string)
    #define kC4ReplicatorProxyHost      "host"           ///< Proxy hostname (string)
    #define kC4ReplicatorProxyPort      "port"           ///< Proxy port number (integer)
    #define kC4ReplicatorProxyAuth      "auth"           ///< Proxy auth (Dict); see above

    // [4]: proxy.type values:
    #define kC4ProxyTypeNone            "none"           ///< Use no proxy (overrides system setting)
    #define kC4ProxyTypeHTTP            "HTTP"           ///< HTTP proxy (using CONNECT method)
    #define kC4ProxyTypeHTTPS           "HTTPS"          ///< HTTPS proxy (using CONNECT method)
    #define kC4ProxyTypeSOCKS           "SOCKS"          ///< SOCKS proxy

    /** @} */

#ifdef __cplusplus
}
#endif

C4_ASSUME_NONNULL_END
