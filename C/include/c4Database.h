//
// c4Database.h
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#include "c4Base.h"

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Database Databases
        @{ */


    //////// CONFIGURATION:

    /** \name Configuration
        @{ */


    /** Boolean options for C4DatabaseConfig. */
    typedef C4_OPTIONS(uint32_t, C4DatabaseFlags) {
        kC4DB_Create        = 1,    ///< Create the file if it doesn't exist
        kC4DB_ReadOnly      = 2,    ///< Open file read-only
        kC4DB_AutoCompact   = 4,    ///< Enable auto-compaction
//      kC4DB_Bundled       = 8,    // OBSOLETE; all dbs are now bundled
        kC4DB_SharedKeys    = 0x10, ///< Enable shared-keys optimization at creation time
        kC4DB_NoUpgrade     = 0x20, ///< Disable upgrading an older-version database
        kC4DB_NonObservable = 0x40, ///< Disable c4DatabaseObserver
    };

    /** Document versioning system (also determines database storage schema) */
    typedef C4_ENUM(uint32_t, C4DocumentVersioning) {
        kC4RevisionTrees,           ///< Revision trees
    };

    /** Encryption algorithms. */
    typedef C4_ENUM(uint32_t, C4EncryptionAlgorithm) {
        kC4EncryptionNone = 0,      ///< No encryption (default)
        kC4EncryptionAES128,        ///< AES with 128-bit key
        kC4EncryptionAES256,        ///< AES with 256-bit key
    };

    /** Encryption key sizes (in bytes). */
    typedef C4_ENUM(uint64_t, C4EncryptionKeySize) {
        kC4EncryptionKeySizeAES128 = 16,
        kC4EncryptionKeySizeAES256 = 32,
    };

    /** Encryption key specified in a C4DatabaseConfig. */
    typedef struct C4EncryptionKey {
        C4EncryptionAlgorithm algorithm;
        uint8_t bytes[32];
    } C4EncryptionKey;

    /** Underlying storage engines that can be used. */
    typedef const char* C4StorageEngine;
    CBL_CORE_API extern C4StorageEngine const kC4SQLiteStorageEngine;

    /** Main database configuration struct. */
    typedef struct C4DatabaseConfig {
        C4DatabaseFlags flags;          ///< Create, ReadOnly, AutoCompact, Bundled...
        C4StorageEngine storageEngine;  ///< Which storage to use, or NULL for no preference
        C4DocumentVersioning versioning;///< Type of document versioning
        C4EncryptionKey encryptionKey;  ///< Encryption to use creating/opening the db
    } C4DatabaseConfig;


    /** @} */

    //////// DATABASE API:

    
    /** Filename extension of databases -- ".cblite2". Includes the period. */
    CBL_CORE_API extern const char* const kC4DatabaseFilenameExtension;


    /** Opaque handle to an opened database. */
    typedef struct c4Database C4Database;


    /** \name Lifecycle
        @{ */

    /** Opens a database. */
    C4Database* c4db_open(C4String path,
                          const C4DatabaseConfig *config C4NONNULL,
                          C4Error *outError) C4API;

    /** Opens a new handle to the same database file as `db`.
        The new connection is completely independent and can be used on another thread. */
    C4Database* c4db_openAgain(C4Database* db C4NONNULL,
                               C4Error *outError) C4API;
    
    /** Copies a prebuilt database from the given source path and places it in the destination
        path.  If a database already exists at that directory then it will be overwritten.  
        However if there is a failure, the original database will be restored as if nothing
        happened */
    bool c4db_copy(C4String sourcePath,
                   C4String destinationPath,
                   const C4DatabaseConfig* config C4NONNULL,
                   C4Error* error) C4API;

    /** Increments the reference count of the database handle. The next call to
        c4db_free() will have no effect. Therefore calls to c4db_retain must be balanced by calls
        to c4db_free, to avoid leaks. */
    C4Database* c4db_retain(C4Database* db);

    /** Frees a database handle, closing the database first if it's still open.
        (More precisely, this decrements the handle's reference count. The handle is only freed
        if the count reaches zero, which it will unless c4db_retain has previously been called.) */
    bool c4db_free(C4Database* database) C4API;

    /** Closes the database. Does not free the handle, although any operation other than
        c4db_free() will fail with an error. */
    bool c4db_close(C4Database* database, C4Error *outError) C4API;

    /** Closes the database and deletes the file/bundle. Does not free the handle, although any
        operation other than c4db_free() will fail with an error. */
    bool c4db_delete(C4Database* database C4NONNULL, C4Error *outError) C4API;

    /** Deletes the file(s) for the database at the given path.
        All C4Databases at that path must be closed first or an error will result.
        Returns false, with no error, if the database doesn't exist. */
    bool c4db_deleteAtPath(C4String dbPath, C4Error *outError) C4API;


    /** Changes a database's encryption key (removing encryption if it's NULL.) */
    bool c4db_rekey(C4Database* database C4NONNULL,
                    const C4EncryptionKey *newKey,
                    C4Error *outError) C4API;

    /** Closes down the storage engines. Must close all databases first.
        You don't generally need to do this, but it can be useful in tests. */
    bool c4_shutdown(C4Error *outError) C4API;


    /** @} */
    /** \name Accessors
        @{ */


    /** Returns the path of the database. */
    C4StringResult c4db_getPath(C4Database* C4NONNULL) C4API;

    /** Returns the configuration the database was opened with. */
    const C4DatabaseConfig* c4db_getConfig(C4Database* C4NONNULL) C4API;

    /** Returns the number of (undeleted) documents in the database. */
    uint64_t c4db_getDocumentCount(C4Database* database C4NONNULL) C4API;

    /** Returns the latest sequence number allocated to a revision. */
    C4SequenceNumber c4db_getLastSequence(C4Database* database C4NONNULL) C4API;

    /** Returns the timestamp at which the next document expiration should take place. */
    uint64_t c4db_nextDocExpiration(C4Database *database C4NONNULL) C4API;

    /** Returns the number of revisions of a document that are tracked. (Defaults to 20.) */
    uint32_t c4db_getMaxRevTreeDepth(C4Database *database C4NONNULL) C4API;

    /** Configures the number of revisions of a document that are tracked. */
    void c4db_setMaxRevTreeDepth(C4Database *database C4NONNULL, uint32_t maxRevTreeDepth) C4API;

    typedef struct {
        uint8_t bytes[16];
    } C4UUID;

    /** Returns the database's public and/or private UUIDs. (Pass NULL for ones you don't want.) */
    bool c4db_getUUIDs(C4Database* database C4NONNULL,
                       C4UUID *publicUUID, C4UUID *privateUUID,
                       C4Error *outError) C4API;


    /** @} */
    /** \name Compaction
        @{ */


    /** Manually compacts the database. */
    bool c4db_compact(C4Database* database C4NONNULL, C4Error *outError) C4API;


    /** @} */
    /** \name Transactions
        @{ */


    /** Begins a transaction.
        Transactions can nest; only the first call actually creates a database transaction. */
    bool c4db_beginTransaction(C4Database* database C4NONNULL,
                               C4Error *outError) C4API;

    /** Commits or aborts a transaction. If there have been multiple calls to beginTransaction, it
        takes the same number of calls to endTransaction to actually end the transaction; only the
        last one commits or aborts the database transaction. */
    bool c4db_endTransaction(C4Database* database C4NONNULL,
                             bool commit,
                             C4Error *outError) C4API;

    /** Is a transaction active? */
    bool c4db_isInTransaction(C4Database* database C4NONNULL) C4API;

    
    /** @} */
    /** @} */


    //////// RAW DOCUMENTS (i.e. info or _local)


    /** \defgroup RawDocs Raw Documents
        @{ */


    /** Contents of a raw document. */
    typedef struct {
        C4String key;    ///< The key (document ID)
        C4String meta;   ///< Metadata (usage is up to the caller)
        C4String body;   ///< Body data
    } C4RawDocument;

    /** Frees the storage occupied by a raw document. */
    void c4raw_free(C4RawDocument* rawDoc) C4API;

    /** Reads a raw document from the database. In Couchbase Lite the store named "info" is used
        for per-database key/value pairs, and the store "_local" is used for local documents. */
    C4RawDocument* c4raw_get(C4Database* database C4NONNULL,
                             C4String storeName,
                             C4String docID,
                             C4Error *outError) C4API;

    /** Writes a raw document to the database, or deletes it if both meta and body are NULL. */
    bool c4raw_put(C4Database* database C4NONNULL,
                   C4String storeName,
                   C4String key,
                   C4String meta,
                   C4String body,
                   C4Error *outError) C4API;

    // Store used for database metadata.
    #define kC4InfoStore C4STR("info")

    // Store used for local (non-replicated) documents.
    #define kC4LocalDocStore C4STR("_local")

    /** @} */
#ifdef __cplusplus
}
#endif
