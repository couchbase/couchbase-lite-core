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
#include "Fleece.h"

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Replicator Replicator
        @{ */

#define kC4Replicator2Scheme    C4STR("blip")
#define kC4Replicator2TLSScheme C4STR("blips")

    /** How to replicate, in either direction */
    typedef C4_ENUM(int32_t, C4ReplicatorMode) {
        kC4Disabled,        // Do not allow this direction
        kC4Passive,         // Allow peer to initiate this direction
        kC4OneShot,         // Replicate, then stop
        kC4Continuous       // Keep replication active until stopped by application
    };

    typedef C4_ENUM(int32_t, C4ReplicatorActivityLevel) {
        kC4Stopped,
        kC4Offline,
        kC4Connecting,
        kC4Idle,
        kC4Busy
    };

    CBL_CORE_API extern const char* const kC4ReplicatorActivityLevelNames[5];


    typedef struct {
        uint64_t    unitsCompleted;
        uint64_t    unitsTotal;
        uint64_t    documentCount;
    } C4Progress;

    /** Current status of replication. Passed to callback. */
    typedef struct {
        C4ReplicatorActivityLevel level;
        C4Progress progress;
        C4Error error;
    } C4ReplicatorStatus;


    /** Opaque reference to a replicator. */
    typedef struct C4Replicator C4Replicator;

    /** Callback a client can register, to get progress information.
        This will be called on arbitrary background threads, and should not block. */
    typedef void (*C4ReplicatorStatusChangedCallback)(C4Replicator* C4NONNULL,
                                                      C4ReplicatorStatus,
                                                      void *context);

    /** Callback a client can register, to hear about errors replicating individual documents. */
    typedef void (*C4ReplicatorDocumentErrorCallback)(C4Replicator* C4NONNULL,
                                                      bool pushing,
                                                      C4String docID,
                                                      C4Error error,
                                                      bool transient,
                                                      void *context);

    /** Callback that can choose to reject an incoming pulled revision by returning false. */
    typedef bool (*C4ReplicatorValidationFunction)(C4String docID,
                                                   FLDict body,
                                                   void* context);

    /** Checks whether a database name is valid, for purposes of appearing in a replication URL */
    bool c4repl_isValidDatabaseName(C4String dbName);

    /** A simple URL parser that populates a C4Address from a URL string.
        The fields of the address will point inside the url string. */
    bool c4repl_parseURL(C4String url,
                         C4Address *address C4NONNULL,
                         C4String *dbName C4NONNULL);


    typedef struct {
        C4ReplicatorMode                  push;              ///< Push mode (from db to remote/other db)
        C4ReplicatorMode                  pull;              ///< Pull mode (from db to remote/other db).
        C4Slice                           optionsDictFleece; ///< Optional Fleece-encoded dictionary of optional parameters.
        C4ReplicatorValidationFunction    validationFunc;    ///< Callback that can reject incoming revisions
        C4ReplicatorStatusChangedCallback onStatusChanged;   ///< Callback to be invoked when replicator's status changes.
        C4ReplicatorDocumentErrorCallback onDocumentError;   ///< Callback notifying of errors with individual documents
        void*                             callbackContext;   ///< Value to be passed to the callbacks.
    } C4ReplicatorParameters;


    /** Creates a new replicator.
        @param db  The local database.
        @param remoteAddress  The address of the remote server (null if other db is local.)
        @param remoteDatabaseName  The name of the database at the remote address.
        @param otherLocalDB  The other local database (null if other db is remote.)
        @param params Replication parameters (see above.)
        @param err  Error, if replication can't be created.
        @return  The newly created replication, or NULL on error. */
    C4Replicator* c4repl_new(C4Database* db C4NONNULL,
                             C4Address remoteAddress,
                             C4String remoteDatabaseName,
                             C4Database* otherLocalDB,
                             C4ReplicatorParameters params,
                             C4Error *err) C4API;

    /** Frees a replicator reference. If the replicator is running it will stop. */
    void c4repl_free(C4Replicator* repl) C4API;

    /** Tells a replicator to stop. */
    void c4repl_stop(C4Replicator* repl C4NONNULL) C4API;

    /** Returns the current state of a replicator. */
    C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl C4NONNULL) C4API;

    /** Returns the HTTP response headers as a Fleece-encoded dictionary. */
    C4Slice c4repl_getResponseHeaders(C4Replicator *repl C4NONNULL) C4API;


#pragma mark - COOKIES:


    /** Takes the value of a "Set-Cookie:" header, received from the given host, and saves the
        cookie into the database's cookie store. (Persistent cookies are saved as metadata in the
        database file until they expire. Session cookies are kept in memory, until the last
        C4Database handle to the given database is closed.) */
    bool c4db_setCookie(C4Database *db C4NONNULL,
                        C4String setCookieHeader,
                        C4String fromHost,
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
    #define kC4ReplicatorOptionExtraHeaders   "headers"  // Extra HTTP headers; string[]
    #define kC4ReplicatorOptionCookies        "cookies"  // HTTP Cookie header value; string
    #define kC4ReplicatorOptionAuthentication "auth"     // Auth settings; Dict
    #define kC4ReplicatorOptionPinnedServerCert "pinnedCert"  // Cert or public key; data
    #define kC4ReplicatorOptionDocIDs         "docIDs"   // Docs to replicate; string[]
    #define kC4ReplicatorOptionChannels       "channels" // SG channel names; string[]
    #define kC4ReplicatorOptionFilter         "filter"   // Filter name; string
    #define kC4ReplicatorOptionFilterParams   "filterParams"  // Filter params; Dict[string]
    #define kC4ReplicatorOptionSkipDeleted    "skipDeleted" // Don't push/pull tombstones; bool
    #define kC4ReplicatorOptionNoIncomingConflicts "noIncomingConflicts" // Reject incoming conflicts; bool
    #define kC4ReplicatorOptionOutgoingConflicts "outgoingConflicts" // Allow creating conflicts on remote; bool
    #define kC4ReplicatorCheckpointInterval   "checkpointInterval" // How often to checkpoint, in seconds; number
    #define kC4ReplicatorOptionRemoteDBUniqueID "remoteDBUniqueID" // Stable ID for remote db with unstable URL; string
    #define kC4ReplicatorHeartbeatInterval    "heartbeat" // Interval in secs to send a keepalive ping

    // Auth dictionary keys:
    #define kC4ReplicatorAuthType       "type"           // Auth property; string
    #define kC4ReplicatorAuthUserName   "username"       // Auth property; string
    #define kC4ReplicatorAuthPassword   "password"       // Auth property; string
    #define kC4ReplicatorAuthClientCert "clientCert"     // Auth property; value platform-dependent

    // auth.type values:
    #define kC4AuthTypeBasic            "Basic"          // HTTP Basic (the default)
    #define kC4AuthTypeSession          "Session"        // SG session cookie
    #define kC4AuthTypeOpenIDConnect    "OpenID Connect"
    #define kC4AuthTypeFacebook         "Facebook"
    #define kC4AuthTypeClientCert       "Client Cert"

    /** @} */

#ifdef __cplusplus
}
#endif
