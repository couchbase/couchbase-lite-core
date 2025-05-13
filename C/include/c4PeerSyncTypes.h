//
// c4PeerSyncTypes.h
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.h"
#include "c4DatabaseTypes.h"
#include "c4ReplicatorTypes.h"

#ifdef COUCHBASE_ENTERPRISE
C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/* Protocol ID constants for use with `C4PeerSyncParameters.protocols`. */
CBL_CORE_API extern const C4String kPeerSyncProtocol_DNS_SD;       ///< DNS-SD ("Bonjour") protocol over IP.
CBL_CORE_API extern const C4String kPeerSyncProtocol_BluetoothLE;  ///< Bluetooth LE protocol with L2CAP. (UNAVAILABLE)

/** The unique ID of a peer, derived from its X.509 certificate.
    (It's technically a SHA256 digest, not a UUID, but we sometimes call it a UUID.)
    A `C4PeerID` is not tied to a single discovery protocol but is shared across them. */
typedef struct C4PeerID {
    uint8_t bytes[32];
} C4PeerID;

/** Callback that notifies that C4PeerSync has started, failed to start, or stopped. */
typedef void (*C4PeerSync_StatusCallback)(C4PeerSync*,   ///< Sender
                                          bool started,  ///< Whether it's running or not
                                          C4Error,       ///< Error, if any
                                          void* C4NULLABLE context);

/** Callback that notifies that a peer has been discovered, or is no longer visible. */
typedef void (*C4PeerSync_DiscoveryCallback)(C4PeerSync*,              ///< Sender
                                             const C4PeerID*,          ///< Peer's ID
                                             bool             online,  ///< Is peer online?
                                             void* C4NULLABLE context);

/** Callback that authenticates a peer based on its X.509 certificate.
    This is not called when a peer is discovered, only when making a direct connection.
    It should return `true` to allow the connection, `false` to prevent it. */
typedef bool (*C4PeerSync_AuthenticatorCallback)(C4PeerSync*,      ///< Sender
                                                 const C4PeerID*,  ///< Peer's ID
                                                 C4Cert*,          ///< Peer's X.509 certificate
                                                 void* C4NULLABLE context);

/** Callback that notifies the status of an individual replication with one peer.
    @note This is similar to \ref C4ReplicatorStatusChangedCallback, but adds the peer's ID
          and indicates whether I connected to the peer or vice versa (just in case you care.) */
typedef void (*C4PeerSync_ReplicatorCallback)(C4PeerSync*,                ///< Sender
                                              const C4PeerID*,            ///< Peer's ID
                                              bool outgoing,              ///< True if I opened the socket
                                              const C4ReplicatorStatus*,  ///< Status/progress
                                              void* C4NULLABLE context);

/** Callback that notifies that documents have been pushed or pulled.
    @note This is similar to \ref C4ReplicatorDocumentsEndedCallback, but adds the peer's ID. */
typedef void (*C4PeerSync_DocsCallback)(C4PeerSync*,                  ///< Sender
                                        const C4PeerID*,              ///< Peer ID
                                        bool                pushing,  ///< Direction of sync
                                        size_t              numDocs,  ///< Size of docs[]
                                        C4DocumentEndedList docs,     ///< Document info
                                        void* C4NULLABLE    context);

/** Callback that notifies about progress pushing or pulling a single blob.
    @note This is similar to \ref C4ReplicatorBlobProgressCallback, but adds the peer's ID. */
typedef void (*C4PeerSync_BlobCallback)(C4PeerSync*,            ///< Sender
                                        const C4PeerID*,        ///< Peer ID
                                        bool pushing,           ///< Direction of transfer
                                        const C4BlobProgress*,  ///< Progress info
                                        void* C4NULLABLE context);

/** Callbacks from C4PeerSync. (See the above typedefs for details of each field.) */
typedef struct C4PeerSyncCallbacks {
    C4PeerSync_StatusCallback                         syncStatus;
    C4PeerSync_AuthenticatorCallback                  authenticator;
    C4PeerSync_DiscoveryCallback C4NULLABLE           onPeerDiscovery;
    C4PeerSync_ReplicatorCallback C4NULLABLE          onReplicatorStatusChanged;
    C4PeerSync_DocsCallback C4NULLABLE                onDocumentsEnded;
    C4PeerSync_BlobCallback C4NULLABLE                onBlobProgress;
    C4ReplicatorPropertyEncryptionCallback C4NULLABLE propertyEncryptor;
    C4ReplicatorPropertyDecryptionCallback C4NULLABLE propertyDecryptor;
    void* C4NULLABLE                                  context;  ///< Value to be passed to the callbacks.
} C4PeerSyncCallbacks;

/** Replicator document validation / filtering callback.
    @note This is similar to \ref C4ReplicatorValidationFunction, but adds the peer's ID. */
typedef bool (*C4PeerSync_ValidationFunction)(C4PeerSync*,                ///< Sender
                                              const C4PeerID*,            ///< Peer's ID
                                              C4CollectionSpec,           ///< Collection
                                              C4String docID,             ///< Document ID
                                              C4String revID,             ///< Revision ID
                                              C4RevisionFlags,            ///< Revision flags
                                              FLDict           body,      ///< Document body
                                              void* C4NULLABLE context);  ///< Callback context

/** Per-collection options for C4PeerSync. (Similar to \ref C4ReplicationCollection) */
typedef struct C4PeerSyncCollection {
    C4CollectionSpec collection;                          ///< Name & scope of collection
    bool             pushEnabled;                         ///< Send documents to peers? (i.e. allow read access)
    bool             pullEnabled;                         ///< Receive documents from peers? (i.e. allow write access)
    C4Slice          optionsDictFleece;                   ///< Per-collection options, like kC4ReplicatorOptionDocIDs
    C4PeerSync_ValidationFunction C4NULLABLE pushFilter;  ///< Callback that can reject outgoing revisions
    C4PeerSync_ValidationFunction C4NULLABLE pullFilter;  ///< Callback that can reject incoming revisions
    void* C4NULLABLE                         callbackContext;  ///< Value to be passed to the callbacks.
} C4PeerSyncCollection;

/** Top-level configuration for creating a C4PeerSync object. */
typedef struct C4PeerSyncParameters {
    C4String                   peerGroupID;        ///< App identifier for peer discovery
    C4String const* C4NULLABLE protocols;          ///< Array of protocols to use (empty means all)
    size_t                     protocolsCount;     ///< Size of protocols array
    C4Cert*                    tlsCert;            ///< My TLS certificate (server+client)
    C4KeyPair*                 tlsKeyPair;         ///< Certificate's key-pair
    C4Database*                database;           ///< Database to sync
    C4PeerSyncCollection*      collections;        ///< Collections to sync
    size_t                     collectionCount;    ///< Size of collections[]
    C4Slice                    optionsDictFleece;  ///< Optional Fleece-encoded dictionary of replicator options
    C4ReplicatorProgressLevel  progressLevel;      ///< Level of detail in replicator callbacks
    C4PeerSyncCallbacks        callbacks;          ///< Client callbacks
} C4PeerSyncParameters;

/** Information about a peer, returned from \ref c4peersync_getPeerInfo.
    @note  References must be freed by calling \ref c4peerinfo_free. */
typedef struct C4PeerInfo {
    C4Cert* C4NULLABLE   certificate;
    C4PeerID* C4NULLABLE neighbors;
    size_t               neighborCount;
    C4ReplicatorStatus   replicatorStatus;
    bool                 online;
} C4PeerInfo;

C4API_END_DECLS
C4_ASSUME_NONNULL_END
#endif  // COUCHBASE_ENTERPRISE
