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
#include "c4Socket.h"
#include "c4Database.h"
#include "c4Document.h"
#include "c4BlobStore.h"
#include "fleece/Fleece.h"

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
        kC4Stopped,     ///< Finished, or got a fatal error.
        kC4Offline,     ///< Not used by LiteCore; for use by higher-level APIs. */
        kC4Connecting,  ///< Connection is in progress.
        kC4Idle,        ///< Continuous replicator has caught up and is waiting for changes.
        kC4Busy         ///< Connected and actively working.
    };

    /** For convenience, an array of C strings naming the C4ReplicatorActivityLevel values. */
    CBL_CORE_API extern const char* const kC4ReplicatorActivityLevelNames[5];


    /** Represents the current progress of a replicator.
        The `units` fields should not be used directly, but divided (`unitsCompleted`/`unitsTotal`)
        to give a _very_ approximate progress fraction. */
    typedef struct {
        uint64_t    unitsCompleted;     ///< Abstract number of work units completed so far
        uint64_t    unitsTotal;         ///< Total number of work units (a very rough approximation)
        uint64_t    documentCount;      ///< Number of documents transferred so far
    } C4Progress;

    /** Current status of replication. Passed to `C4ReplicatorStatusChangedCallback`. */
    typedef struct {
        C4ReplicatorActivityLevel level;
        C4Progress progress;
        C4Error error;
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


    /** Opaque reference to a replicator. */
    typedef struct C4Replicator C4Replicator;

    /** Callback a client can register, to get progress information.
        This will be called on arbitrary background threads, and should not block. */
    typedef void (*C4ReplicatorStatusChangedCallback)(C4Replicator* C4NONNULL,
                                                      C4ReplicatorStatus,
                                                      void *context);

    /** Callback a client can register, to hear about the replication status of documents.
        By default, only errors will be reported via this callback.
        To also receive callbacks for successfully completed documents, set the
        kC4ReplicatorOptionProgressLevel option to a value greater than zero. */
    typedef void (*C4ReplicatorDocumentsEndedCallback)(C4Replicator* C4NONNULL,
                                                       bool pushing,
                                                       size_t numDocs,
                                                       const C4DocumentEnded* docs[],
                                                       void *context);

    /** Callback a client can register, to hear about the status of blobs. */
    typedef void (*C4ReplicatorBlobProgressCallback)(C4Replicator* C4NONNULL,
                                                     bool pushing,
                                                     C4String docID,
                                                     C4String docProperty,
                                                     C4BlobKey blobKey,
                                                     uint64_t bytesComplete,
                                                     uint64_t bytesTotal,
                                                     C4Error error,
                                                     void *context);

    /** Callback that can choose to reject an incoming pulled revision, or stop a local
        revision from being pushed, by returning false.
        (Note: In the case of an incoming revision, no flags other than 'deletion' and
        'hasAttachments' will be set.) */
    typedef bool (*C4ReplicatorValidationFunction)(C4String docID,
                                                   C4RevisionFlags,
                                                   FLDict body,
                                                   void* context);

    /** Checks whether a database name is valid, for purposes of appearing in a replication URL */
    bool c4repl_isValidDatabaseName(C4String dbName) C4API;

    /** Checks whether the destination of a replication is valid.
        (c4repl_new makes the same checks; this function is exposed so CBL can fail sooner.) */
    bool c4repl_isValidRemote(C4Address remoteAddress,
                              C4String remoteDatabaseName,
                              C4Error *outError) C4API;

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
                           C4Address *address C4NONNULL,
                           C4String *dbName) C4API;

    /** Converts a C4Address to a URL. */
    C4StringResult c4address_toURL(C4Address address) C4API;


    /** Parameters describing a replication, used when creating a C4Replicator. */
    typedef struct {
        C4ReplicatorMode                    push;              ///< Push mode (from db to remote/other db)
        C4ReplicatorMode                    pull;              ///< Pull mode (from db to remote/other db).
        C4Slice                             optionsDictFleece; ///< Optional Fleece-encoded dictionary of optional parameters.
        C4ReplicatorValidationFunction      pushFilter;        ///< Callback that can reject outgoing revisions
        C4ReplicatorValidationFunction      validationFunc;    ///< Callback that can reject incoming revisions
        C4ReplicatorStatusChangedCallback   onStatusChanged;   ///< Callback to be invoked when replicator's status changes.
        C4ReplicatorDocumentsEndedCallback  onDocumentsEnded;  ///< Callback notifying status of individual documents
        C4ReplicatorBlobProgressCallback    onBlobProgress;    ///< Callback notifying blob progress
        void*                               callbackContext;   ///< Value to be passed to the callbacks.
        const C4SocketFactory*              socketFactory;     ///< Custom C4SocketFactory, if not NULL
        bool                                dontStart;         ///< Don't start automatically
    } C4ReplicatorParameters;


    /** Creates a new replicator.
        @param db  The local database.
        @param remoteAddress  The address of the remote server (null if other db is local.)
        @param remoteDatabaseName  The name of the database at the remote address.
        @param otherLocalDB  The other local database (null if other db is remote.)
        @param params Replication parameters (see above.)
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
    C4Replicator* c4repl_new(C4Database* db C4NONNULL,
                             C4Address remoteAddress,
                             C4String remoteDatabaseName,
                             C4Database* otherLocalDB,
                             C4ReplicatorParameters params,
                             C4Error *outError) C4API;

    /** Creates a new replicator from an already-open C4Socket. This is for use by listeners
        that accept incoming connections, wrap them by calling `c4socket_fromNative()`, then
        start a passive replication to service them.
        @param db  The local database.
        @param openSocket  An already-created C4Socket.
        @param params  Replication parameters. Will usually use kC4Passive modes.
        @param outError  Error, if replication can't be created.
        @return  The newly created replicator, or NULL on error. */
    C4Replicator* c4repl_newWithSocket(C4Database* db C4NONNULL,
                                       C4Socket *openSocket C4NONNULL,
                                       C4ReplicatorParameters params,
                                       C4Error *outError) C4API;

    /** Frees a replicator reference.
        Does not stop the replicator -- if the replicator still has other internal references,
        it will keep going. If you need the replicator to stop, call `c4repl_stop()` first. */
    void c4repl_free(C4Replicator* repl) C4API;

    /** Tells a replicator to start.
        **Only call this if you set \ref dontStart in the \ref C4ReplicatorParameters !!** */
    void c4repl_start(C4Replicator* repl C4NONNULL) C4API;

    /** Tells a replicator to stop. */
    void c4repl_stop(C4Replicator* repl C4NONNULL) C4API;

    /** Returns the current state of a replicator. */
    C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl C4NONNULL) C4API;

    /** Returns the HTTP response headers as a Fleece-encoded dictionary. */
    C4Slice c4repl_getResponseHeaders(C4Replicator *repl C4NONNULL) C4API;

    /** Gets a fleece encoded list of IDs of documents who have revisions pending push.  This
     *  API is a snapshot and results may change between the time the call was made and the time
     *  the call returns.
     *
     *  @param outErr Records error information, if any
     *  @return A fleece encoded array of document IDs, each of which has one or more pending
     *  revisions
     */
    C4SliceResult c4repl_getPendingDocIDs(C4Replicator* repl C4NONNULL, C4Error* outErr) C4API;

    /** Checks if the document with the given ID has revisions pending push.  This
     *  API is a snapshot and results may change between the time the call was made and the time
     *  the call returns.
     *
     * @param docID The ID of the document to check
     * @param outErr Records error information, if any
     * @return true if the document has one or more revisions pending, false otherwise
     */
    bool c4repl_isDocumentPending(C4Replicator* repl C4NONNULL, C4String docID, C4Error* outErr) C4API;


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
                        C4Error *outError) C4API;

    /** Locates any saved HTTP cookies relevant to the given request, and returns them as a string
        that can be used as the value of a "Cookie:" header. */
    C4StringResult c4db_getCookies(C4Database *db C4NONNULL,
                                   C4Address request,
                                   C4Error *error) C4API;

    /** Removes all cookies from the database's cookie store. */
    void c4db_clearCookies(C4Database *db C4NONNULL) C4API;


#pragma mark - CONSTANTS:


    // Replicator option dictionary keys:
    #define kC4ReplicatorOptionExtraHeaders     "headers"  ///< Extra HTTP headers (string[])
    #define kC4ReplicatorOptionCookies          "cookies"  ///< HTTP Cookie header value (string)
    #define kC4ReplicatorOptionAuthentication   "auth"     ///< Auth settings (Dict)
    #define kC4ReplicatorOptionPinnedServerCert "pinnedCert"  ///< Cert or public key (data)
    #define kC4ReplicatorOptionDocIDs           "docIDs"   ///< Docs to replicate (string[])
    #define kC4ReplicatorOptionChannels         "channels" ///< SG channel names (string[])
    #define kC4ReplicatorOptionFilter           "filter"   ///< Pull filter name (string)
    #define kC4ReplicatorOptionFilterParams     "filterParams"  ///< Pull filter params (Dict[string])
    #define kC4ReplicatorOptionSkipDeleted      "skipDeleted" ///< Don't push/pull tombstones (bool)
    #define kC4ReplicatorOptionNoIncomingConflicts "noIncomingConflicts" ///< Reject incoming conflicts (bool)
    #define kC4ReplicatorOptionOutgoingConflicts   "outgoingConflicts" ///< Allow creating conflicts on remote (bool)
    #define kC4ReplicatorCheckpointInterval     "checkpointInterval" ///< How often to checkpoint, in seconds (number)
    #define kC4ReplicatorOptionRemoteDBUniqueID "remoteDBUniqueID" ///< Stable ID for remote db with unstable URL (string)
    #define kC4ReplicatorHeartbeatInterval      "heartbeat" ///< Interval in secs to send a keepalive ping
    #define kC4ReplicatorResetCheckpoint        "reset"     ///< Start over w/o checkpoint (bool)
    #define kC4ReplicatorOptionProgressLevel    "progress"  ///< If >=1, notify on every doc; if >=2, on every attachment (int)
    #define kC4ReplicatorOptionDisableDeltas    "noDeltas"   ///< Disables delta sync (bool)

    // Auth dictionary keys:
    #define kC4ReplicatorAuthType       "type"           ///< Auth type; see below (string)
    #define kC4ReplicatorAuthUserName   "username"       ///< User name for basic auth (string)
    #define kC4ReplicatorAuthPassword   "password"       ///< Password for basic auth (string)
    #define kC4ReplicatorAuthClientCert "clientCert"     ///< TLS client certificate (value platform-dependent)
    #define kC4ReplicatorAuthToken      "token"          ///< Session cookie or auth token (string)

    // auth.type values:
    #define kC4AuthTypeBasic            "Basic"          ///< HTTP Basic (the default)
    #define kC4AuthTypeSession          "Session"        ///< SG session cookie
    #define kC4AuthTypeOpenIDConnect    "OpenID Connect" ///< OpenID Connect token
    #define kC4AuthTypeFacebook         "Facebook"       ///< Facebook auth token
    #define kC4AuthTypeClientCert       "Client Cert"    ///< TLS client cert

    /** @} */

#ifdef __cplusplus
}
#endif
