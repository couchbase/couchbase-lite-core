//
//  c4Replicator.h
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4Socket.h"
#include "c4Database.h"

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

    extern const char* const kC4ReplicatorActivityLevelNames[5];


    typedef struct {
        uint64_t    completed;
        uint64_t    total;
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
    typedef void (*C4ReplicatorStatusChangedCallback)(C4Replicator*,
                                                      C4ReplicatorStatus,
                                                      void *context);

    /** Checks whether a database name is valid, for purposes of appearing in a replication URL */
    bool c4repl_isValidDatabaseName(C4String dbName);

    /** A simple URL parser that populates a C4Address from a URL string.
        The fields of the address will point inside the url string. */
    bool c4repl_parseURL(C4String url, C4Address *address, C4String *dbName);

    /** Creates a new replicator.
        @param db  The local database.
        @param remoteAddress  The address of the remote database (null if other db is local.)
        @param otherLocalDB  The other local database (null if other db is remote.)
        @param push  Push mode (from db to remote/other db).
        @param pull  Pull mode (from db to remote/other db).
        @param optionsDictFleece  Optional Fleece-encoded dictionary of optional parameters.
        @param onStatusChanged  Callback to be invoked when replicator's status changes.
        @param callbackContext  Value to be passed to the onStatusChanged callback.
        @param err  Error, if replication can't be created.
        @return  The newly created replication, or NULL on error. */
    C4Replicator* c4repl_new(C4Database* db,
                             C4Address remoteAddress,
                             C4String remoteDatabaseName,
                             C4Database* otherLocalDB,
                             C4ReplicatorMode push,
                             C4ReplicatorMode pull,
                             C4Slice optionsDictFleece,
                             C4ReplicatorStatusChangedCallback onStatusChanged,
                             void *callbackContext,
                             C4Error *err) C4API;

    /** Frees a replicator reference. If the replicator is running it will stop. */
    void c4repl_free(C4Replicator* repl) C4API;

    /** Tells a replicator to stop. */
    void c4repl_stop(C4Replicator* repl) C4API;

    /** Returns the current state of a replicator. */
    C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl) C4API;

    /** Returns the HTTP response headers as a Fleece-encoded dictionary. */
    C4Slice c4repl_getResponseHeaders(C4Replicator *repl) C4API;


#pragma mark - COOKIES:


    /** Takes the value of a "Set-Cookie:" header, received from the given host, and saves the
        cookie into the database's cookie store. (Persistent cookies are saved as metadata in the
        database file until they expire. Session cookies are kept in memory, until the last
        C4Database handle to the given database is closed.) */
    bool c4db_setCookie(C4Database *db,
                        C4String setCookieHeader,
                        C4String fromHost,
                        C4Error *outError) C4API;

    /** Locates any saved HTTP cookies relevant to the given request, and returns them as a string
        that can be used as the value of a "Cookie:" header. */
    C4StringResult c4db_getCookies(C4Database *db,
                                   C4Address request,
                                   C4Error *error) C4API;

    /** Removes all cookies from the database's cookie store. */
    void c4db_clearCookies(C4Database *db) C4API;


#pragma mark - ERRORS:


    /** Returns true if this is a network error that may be transient,
        i.e. the client should retry after a delay. */
    bool c4error_mayBeTransient(C4Error err) C4API;

    /** Returns true if this error might go away when the network environment changes,
        i.e. the client should retry after notification of a network status change. */
    bool c4error_mayBeNetworkDependent(C4Error err) C4API;


#pragma mark - CONSTANTS:


    // Replicator option dictionary keys:
    #define kC4ReplicatorOptionExtraHeaders   "headers"  // Extra HTTP headers; string[]
    #define kC4ReplicatorOptionCookies        "cookies"  // HTTP Cookie header value; string
    #define kC4ReplicatorOptionAuthentication "auth"     // Auth settings; Dict
    #define kC4ReplicatorOptionPinnedServerCert "pinnedCert"  // Cert or public key [data]
    #define kC4ReplicatorOptionChannels       "channels" // SG channel names; string[]
    #define kC4ReplicatorOptionFilter         "filter"   // Filter name; string
    #define kC4ReplicatorOptionFilterParams   "filterParams"  // Filter params; Dict[string]
    #define kC4ReplicatorOptionSkipDeleted    "skipDeleted" // Don't push/pull tombstones; bool

    // Auth dictionary keys:
    #define kC4ReplicatorAuthType       "type"           // Auth property; string
    #define kC4ReplicatorAuthUserName   "username"       // Auth property; string
    #define kC4ReplicatorAuthPassword   "password"       // Auth property; string

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
