//
// c4ReplicatorTypes.h
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
#include "c4DatabaseTypes.h"
#include "c4DocumentTypes.h"
#include "fleece/Fleece.h"

#ifdef __cplusplus
#include "fleece/slice.hh"
#endif

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

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

#if __cplusplus
        bool isValidRemote(fleece::slice withDbName,
                           C4Error* C4NULLABLE =nullptr) const noexcept;
        fleece::alloc_slice toURL() const;
        static bool fromURL(fleece::slice url,
                            C4Address *outAddress,
                            fleece::slice* C4NULLABLE outDBName);
#endif
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
        C4HeapString collectionName;
        C4HeapString scopeName;
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
                                                     C4CollectionSpec collectionSpec,
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
    typedef bool (*C4ReplicatorValidationFunction)(C4CollectionSpec collectionSpec,
                                                   C4String docID,
                                                   C4String revID,
                                                   C4RevisionFlags,
                                                   FLDict body,
                                                   void* C4NULLABLE context);

#ifdef COUCHBASE_ENTERPRISE
    // jzhao - the following two may need collectionSpec, or what documentID should belong to.
    // Currently I don't see how documentID is used.
    /** Callback that encrypts properties, in documents pushed by the replicator. */
    typedef C4SliceResult (*C4ReplicatorPropertyEncryptionCallback)(
                   void* C4NULLABLE context,    ///< Replicator’s context
                   C4String documentID,         ///< Document’s ID
                   FLDict properties,           ///< Document’s properties
                   C4String keyPath,            ///< Key path of the property to be encrypted
                   C4Slice input,               ///< Property data to be encrypted.
                   C4StringResult* outAlgorithm,///< On return: algorithm name (optional).
                   C4StringResult* outKeyID,    ///< On return: encryption Key Identifier (optional).
                   C4Error* outError);          ///< On return: error domain/code, if returning NULL

    /** Callback that decrypts properties, in documents pulled by the replicator. */
    typedef C4SliceResult (*C4ReplicatorPropertyDecryptionCallback)(
                   void* C4NULLABLE context,    ///< Replicator’s context
                   C4String documentID,         ///< Document’s ID
                   FLDict properties,           ///< Document’s properties
                   C4String keyPath,            ///< Key path of the property to be decrypted
                   C4Slice input,               ///< Encrypted property data for you to decrypt.
                   C4String algorithm,          ///< Algorithm name, if any.
                   C4String keyID,              ///< Encryption Key Identifier, if any.
                   C4Error* outError);          ///< On return: error domain/code, if returning NULL
#else
    typedef void* C4ReplicatorPropertyEncryptionCallback;
    typedef void* C4ReplicatorPropertyDecryptionCallback;
#endif // COUCHBASE_ENTERPRISE


    typedef struct C4ReplicationCollection {
        C4CollectionSpec collection;

        C4ReplicatorMode                    push;              ///< Push mode (from db to remote/other db)
        C4ReplicatorMode                    pull;              ///< Pull mode (from db to remote/other db).

        // Following options should be encoded into the optionsDictFleed per-collection
        //#define kC4ReplicatorOptionDocIDs           "docIDs"   ///< Docs to replicate (string[])
        //#define kC4ReplicatorOptionChannels         "channels" ///< SG channel names (string[])
        //#define kC4ReplicatorOptionFilter           "filter"   ///< Pull filter name (string)
        //#define kC4ReplicatorOptionFilterParams     "filterParams"  ///< Pull filter params (Dict[string])
        //#define kC4ReplicatorOptionSkipDeleted      "skipDeleted" ///< Don't push/pull tombstones (bool)
        //#define kC4ReplicatorOptionNoIncomingConflicts "noIncomingConflicts" ///< Reject incoming conflicts (bool)
        //#define kC4ReplicatorCheckpointInterval     "checkpointInterval" ///< How often to checkpoint, in seconds (number)
        //
        C4Slice                             optionsDictFleece;

    } C4ReplicationCollection;

    /** Parameters describing a replication, used when creating a C4Replicator. */
    typedef struct C4ReplicatorParameters {
        // Begin to be deprecated
        C4ReplicatorMode                    push;              ///< Push mode (from db to remote/other db)
        C4ReplicatorMode                    pull;              ///< Pull mode (from db to remote/other db).
        // End to be deprecated

        C4Slice                             optionsDictFleece; ///< Optional Fleece-encoded dictionary of optional parameters.
        C4ReplicatorValidationFunction C4NULLABLE      pushFilter;        ///< Callback that can reject outgoing revisions
        C4ReplicatorValidationFunction C4NULLABLE      validationFunc;    ///< Callback that can reject incoming revisions
        C4ReplicatorStatusChangedCallback C4NULLABLE   onStatusChanged;   ///< Callback to be invoked when replicator's status changes.
        C4ReplicatorDocumentsEndedCallback C4NULLABLE onDocumentsEnded;  ///< Callback notifying status of individual documents
        C4ReplicatorBlobProgressCallback C4NULLABLE    onBlobProgress;    ///< Callback notifying blob progress
        C4ReplicatorPropertyEncryptionCallback C4NULLABLE propertyEncryptor;
        C4ReplicatorPropertyDecryptionCallback C4NULLABLE propertyDecryptor;
        void* C4NULLABLE                               callbackContext;   ///< Value to be passed to the callbacks.
        const C4SocketFactory* C4NULLABLE              socketFactory;     ///< Custom C4SocketFactory, if not NULL
        // If collections == nullptr, we will use the deprecated fields to build
        // the internal config for one collection being the default collection.
        C4ReplicationCollection             *collections;
        unsigned                            collectionCount;
    } C4ReplicatorParameters;


#pragma mark - CONSTANTS:


    // Replicator option dictionary keys:

    // begins of collection specific properties.
    // That is, they are supposed to be assigned to c4ReplicatorParameters.collections[i].optionsDictFleece
    #define kC4ReplicatorOptionDocIDs           "docIDs"   ///< Docs to replicate (string[])
    #define kC4ReplicatorOptionChannels         "channels" ///< SG channel names (string[])
    #define kC4ReplicatorOptionFilter           "filter"   ///< Pull filter name (string)
    #define kC4ReplicatorOptionFilterParams     "filterParams"  ///< Pull filter params (Dict[string])
    #define kC4ReplicatorOptionSkipDeleted      "skipDeleted" ///< Don't push/pull tombstones (bool)
    #define kC4ReplicatorOptionNoIncomingConflicts "noIncomingConflicts" ///< Reject incoming conflicts (bool)
    #define kC4ReplicatorCheckpointInterval     "checkpointInterval" ///< How often to checkpoint, in seconds (number)
    // end of collection specific properties.

    #define kC4ReplicatorOptionRemoteDBUniqueID "remoteDBUniqueID" ///< Stable ID for remote db with unstable URL (string)
    #define kC4ReplicatorOptionDisableDeltas    "noDeltas"   ///< Disables delta sync (bool)
    #define kC4ReplicatorOptionDisablePropertyDecryption "noDecryption" ///< Disables property decryption (bool)
    #define kC4ReplicatorOptionMaxRetries       "maxRetries" ///< Max number of retry attempts (int)
    #define kC4ReplicatorOptionMaxRetryInterval "maxRetryInterval" ///< Max delay betw retries (secs)
    #define kC4ReplicatorOptionAutoPurge        "autoPurge" ///< Enables auto purge; default is true (bool)

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
    #define kC4ReplicatorHeartbeatInterval      "heartbeat"         ///< Interval in secs to send a keepalive ping
    #define kC4SocketOptionWSProtocols          "WS-Protocols"      ///< Sec-WebSocket-Protocol header value
    #define kC4SocketOptionNetworkInterface     "networkInterface"  ///< Specific network interface (name or IP address) used for connecting to the remote server.

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

C4API_END_DECLS
C4_ASSUME_NONNULL_END
