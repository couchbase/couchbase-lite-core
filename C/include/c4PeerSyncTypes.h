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

typedef C4UUID C4PeerID;

/** Callback that notifies that C4PeerSync has started, failed to start, or stopped. */
typedef void (*C4PeerSync_StatusCallback)(C4PeerSync*,   ///< Sender
                                          bool started,  ///< Whether it's running or not
                                          C4Error,       ///< Error, if any
                                          void* C4NULLABLE context);

/** Callback that notifies that a peer has been discovered, or is no longer visible. */
typedef void (*C4PeerSync_DiscoveryCallback)(C4PeerSync*,              ///< Sender
                                             const C4PeerID*,          ///< Peer's UUID
                                             bool             online,  ///< Is peer online?
                                             void* C4NULLABLE context);

/** Callback that authenticates a peer based on its X.509 certificate.
    This is not called when a peer is discovered, only when making a direct connection. */
typedef bool (*C4PeerSync_AuthenticatorCallback)(C4PeerSync*,      ///< Sender
                                                 const C4PeerID*,  ///< Peer's UUID
                                                 C4Cert*,          ///< Peer's X.509 certificate
                                                 void* C4NULLABLE context);

/** Callback that notifies the status of an individual replication with one peer. */
typedef void (*C4PeerSync_ReplicatorCallback)(C4PeerSync*,                ///< Sender
                                              const C4PeerID*,            ///< Peer's UUID
                                              bool outgoing,              ///< Direction of replicator
                                              const C4ReplicatorStatus*,  ///< Status/progress
                                              void* C4NULLABLE context);
/** Callback that notifies that documents have been pushed or pulled. */
typedef void (*C4PeerSync_DocsCallback)(C4PeerSync*,                  ///< Sender
                                        const C4PeerID*,              ///< Peer UUID
                                        bool                pushing,  ///< Direction of sync
                                        size_t              numDocs,  ///< Size of docs[]
                                        C4DocumentEndedList docs,     ///< Document info
                                        void* C4NULLABLE    context);

/** Callback that notifies about progress pushing or pulling a single blob. */
typedef void (*C4PeerSync_BlobCallback)(C4PeerSync*,            ///< Sender
                                        const C4PeerID*,        ///< Peer UUID
                                        bool pushing,           ///< Direction of transfer
                                        const C4BlobProgress*,  ///< Progress info
                                        void* C4NULLABLE context);

/** Callbacks from C4PeerSync. */
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

/** Configuration for creating a C4PeerSync object. */
typedef struct C4PeerSyncParameters {
    C4String                   peerGroupID;     ///< App identifier for peer discovery
    C4String const* C4NULLABLE protocols;       ///< Array of protocols to use (empty means all)
    size_t                     protocolsCount;  ///< Size of protocols array
    C4Cert*                    tlsCert;         ///< My TLS certificate (server+client)
    C4KeyPair*                 tlsKeyPair;      ///< Certificate's key-pair

    C4Database*              database;           ///< Database to sync
    C4ReplicationCollection* collections;        ///< Collections to sync
    size_t                   collectionCount;    ///< Size of collections[]
    C4Slice                  optionsDictFleece;  ///< Optional Fleece-encoded dictionary of replicator options

    C4ReplicatorProgressLevel progressLevel;  ///< Level of detail in callbacks
    C4PeerSyncCallbacks       callbacks;
} C4PeerSyncParameters;

extern const C4String kPeerSyncProtocol_DNS_SD;       ///< DNS-SD ("Bonjour") protocol over IP.
extern const C4String kPeerSyncProtocol_BluetoothLE;  ///< Bluetooth LE protocol with L2CAP.

C4API_END_DECLS
C4_ASSUME_NONNULL_END
#endif  // COUCHBASE_ENTERPRISE
