//
// c4DatabaseTypes.h
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "c4Base.h"


C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup Database Databases
    @{ */


//////// CONFIGURATION:

/** \name Configuration
    @{ */


/** Boolean options for C4DatabaseConfig. */
typedef C4_OPTIONS(uint32_t, C4DatabaseFlags){
        kC4DB_Create         = 0x01,  ///< Create the file if it doesn't exist
        kC4DB_ReadOnly       = 0x02,  ///< Open file read-only
        kC4DB_AutoCompact    = 0x04,  ///< Enable auto-compaction [UNIMPLEMENTED]
        kC4DB_VersionVectors = 0x08,  ///< Upgrade DB to version vectors instead of rev trees [EXPERIMENTAL]
        kC4DB_NoUpgrade      = 0x20,  ///< Disable upgrading an older-version database
        kC4DB_NonObservable  = 0x40,  ///< Disable database/collection observers, for slightly faster writes
};


/** Encryption algorithms. */
typedef C4_ENUM(uint32_t, C4EncryptionAlgorithm){
        kC4EncryptionNone = 0,  ///< No encryption (default)
        kC4EncryptionAES256,    ///< AES with 256-bit key [ENTERPRISE EDITION ONLY]
};


/** Encryption key sizes (in bytes). */
typedef C4_ENUM(uint64_t, C4EncryptionKeySize){
        kC4EncryptionKeySizeAES256 = 32,
};

/** Encryption key specified in a C4DatabaseConfig. */
typedef struct C4EncryptionKey {
    C4EncryptionAlgorithm algorithm;
    uint8_t               bytes[32];
} C4EncryptionKey;

/** Main database configuration struct (version 2) for use with c4db_openNamed etc. */
typedef struct C4DatabaseConfig2 {
    C4Slice         parentDirectory;  ///< Directory for databases
    C4DatabaseFlags flags;            ///< Flags for opening db, versioning, ...
    C4EncryptionKey encryptionKey;    ///< Encryption to use creating/opening the db
} C4DatabaseConfig2;

/** Filename extension of databases -- ".cblite2". Includes the period. */
CBL_CORE_API extern const char* const kC4DatabaseFilenameExtension;

/** @} */
/** \name Accessors
    @{ */


typedef struct C4UUID {
    uint8_t bytes[16];
} C4UUID;

/** @} */
/** \name Scopes and Collections
    @{ */


#define kC4DefaultScopeID FLSTR("_default")

#define kC4DefaultCollectionName FLSTR("_default")

/** Full identifier of a collection in a database, including its scope. */
typedef struct C4CollectionSpec {
    C4String name;
    C4String scope;
} C4CollectionSpec;

#ifdef __cplusplus
#    define kC4DefaultCollectionSpec (C4CollectionSpec{kC4DefaultCollectionName, kC4DefaultScopeID})
#else
#    define kC4DefaultCollectionSpec ((C4CollectionSpec){kC4DefaultCollectionName, kC4DefaultScopeID})
#endif


/** @} */
/** \name Database Maintenance
    @{ */


/** Types of maintenance that \ref c4db_maintenance can perform. */
typedef C4_ENUM(uint32_t, C4MaintenanceType){
        /// Shrinks the database file by removing any empty pages,
        /// and deletes blobs that are no longer referenced by any documents.
        /// (Runs SQLite `PRAGMA incremental_vacuum; PRAGMA wal_checkpoint(TRUNCATE)`.)
        kC4Compact,

        /// Rebuilds indexes from scratch. Normally never needed, but can be used to help
        /// diagnose/troubleshoot cases of database corruption if only indexes are affected.
        /// (Runs SQLite `REINDEX`.)
        kC4Reindex,

        /// Checks for database corruption, as might be caused by a damaged filesystem, or
        /// memory corruption.
        /// (Runs SQLite `PRAGMA integrity_check`.)
        kC4IntegrityCheck,

        /// Quickly updates database statistics that may help optimize queries that have been run
        /// by this Database since it was opened. The more queries that have been run, the more
        /// effective this will be, but it tries to do its work quickly by scanning only portions
        /// of indexes.
        /// This operation is also performed automatically by \ref c4db_close.
        /// (Runs SQLite `PRAGMA analysis_limit=400; PRAGMA optimize`.)
        kC4QuickOptimize,

        /// Fully scans all indexes to gather database statistics that help optimize queries.
        /// This may take some time, depending on the size of the indexes, but it doesn't have to
        /// be redone unless the database changes drastically, or new indexes are created.
        /// (Runs SQLite `PRAGMA analysis_limit=0; ANALYZE`.)
        kC4FullOptimize,
};  // *NOTE:* These enum values must match the ones in DataFile::MaintenanceType

/** @} */
/** @} */


/** \defgroup RawDocs Raw Documents
    @{ */


/** Contents of a raw document. */
struct C4RawDocument {
    C4String key;   ///< The key (document ID)
    C4String meta;  ///< Metadata (usage is up to the caller)
    C4String body;  ///< Body data
};

/** @} */


//-------- DEPRECATED --------

typedef C4_ENUM(uint32_t, C4DocumentVersioning){
        kC4TreeVersioning_v2,  ///< Revision trees, old v2.x schema
        kC4TreeVersioning,     ///< Revision trees, v3.x schema
        kC4VectorVersioning    ///< Version vectors
};

typedef const char*                       C4StorageEngine;
CBL_CORE_API extern C4StorageEngine const kC4SQLiteStorageEngine;

typedef struct C4DatabaseConfig {
    C4DatabaseFlags            flags;          ///< Create, ReadOnly, AutoCompact, Bundled...
    C4StorageEngine C4NULLABLE storageEngine;  ///< Which storage to use, or NULL for no preference
    C4DocumentVersioning       versioning;     ///< Type of document versioning
    C4EncryptionKey            encryptionKey;  ///< Encryption to use creating/opening the db
} C4DatabaseConfig;

C4API_END_DECLS
C4_ASSUME_NONNULL_END
