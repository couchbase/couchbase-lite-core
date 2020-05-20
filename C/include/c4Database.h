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
        kC4DB_SharedKeys    = 0x10, // OBSOLETE; shared keys are always used
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
        kC4EncryptionAES256,        ///< AES with 256-bit key
    };

    /** Encryption key sizes (in bytes). */
    typedef C4_ENUM(uint64_t, C4EncryptionKeySize) {
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

    /** Main database configuration struct (version 2) for use with c4db_openNamed etc.. */
    typedef struct C4DatabaseConfig2 {
        C4Slice parentDirectory;        ///< Directory for databases
        C4DatabaseFlags flags;          ///< Create, ReadOnly, NoUpgrade (AutoCompact & SharedKeys always set)
        C4EncryptionKey encryptionKey;  ///< Encryption to use creating/opening the db
    } C4DatabaseConfig2;


    /** Stores a password into a C4EncryptionKey, by using the key-derivation algorithm PBKDF2
        to securely convert the password into a raw binary key.
        @param encryptionKey  The raw key will be stored here.
        @param password  The password string.
        @param alg  The encryption algorithm to use. Must not be kC4EncryptionNone.
        @return  True on success, false on failure */
    bool c4key_setPassword(C4EncryptionKey *encryptionKey C4NONNULL,
                           C4String password,
                           C4EncryptionAlgorithm alg) C4API;


    /** Stores a password into a C4EncryptionKey, by using the key-derivation algorithm PBKDF2
        to securely convert the password into a raw binary key.
        @param encryptionKey  The raw key will be stored here.
        @param password  The password string.
        @param alg  The encryption algorithm to use. Must not be kC4EncryptionNone.
        @return  True on success, false on failure */
    bool c4key_setPassword(C4EncryptionKey *encryptionKey C4NONNULL,
                           C4String password,
                           C4EncryptionAlgorithm alg) C4API;


    /** @} */

    //////// DATABASE API:

    
    /** Filename extension of databases -- ".cblite2". Includes the period. */
    CBL_CORE_API extern const char* const kC4DatabaseFilenameExtension;


    /** \name Lifecycle
        @{ */

    /** Returns true if a database with the given name exists in the directory. */
    bool c4db_exists(C4String name, C4String inDirectory) C4API;


    /** Opens a database given its full path. */
    C4Database* c4db_open(C4String path,
                          const C4DatabaseConfig *config C4NONNULL,
                          C4Error *outError) C4API;

    /** Opens a database given its name (without the ".cblite2" extension) and directory. */
    C4Database* c4db_openNamed(C4String name,
                               const C4DatabaseConfig2 *config C4NONNULL,
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

    /** Copies a prebuilt database from the given source path and places it in the destination
        directory, with the given name. If a database already exists there, it will be overwritten.
        However if there is a failure, the original database will be restored as if nothing
        happened.
        @param sourcePath  The path to the database to be copied.
        @param destinationName  The name (without filename extension) of the database to create.
        @param config  Database configuration (including destination directory.)
        @param error  On failure, error info will be written here.
        @return  True on success, false on failure. */
    bool c4db_copyNamed(C4String sourcePath,
                        C4String destinationName,
                        const C4DatabaseConfig2* config C4NONNULL,
                        C4Error* error) C4API;

    /** Closes the database. Does not free the handle, although any operation other than
        c4db_release() will fail with an error. */
    bool c4db_close(C4Database* database, C4Error *outError) C4API;

    /** Closes the database and deletes the file/bundle. Does not free the handle, although any
        operation other than c4db_release() will fail with an error. */
    bool c4db_delete(C4Database* database C4NONNULL, C4Error *outError) C4API;

    /** Deletes the file(s) for the database at the given path.
        All C4Databases at that path must be closed first or an error will result.
        Returns false, with no error, if the database doesn't exist. */
    bool c4db_deleteAtPath(C4String dbPath, C4Error *outError) C4API;

    /** Deletes the file(s) for the database with the given name in the given directory.
        All C4Databases at that path must be closed first or an error will result.
        Returns false, with no error, if the database doesn't exist. */
    bool c4db_deleteNamed(C4String dbName,
                          C4String inDirectory,
                          C4Error *outError) C4API;


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

    /** A fast check that returns true if this database _may_ have expiring documents.
        (The implementation actually checks whether any document in this database has ever had an
        expiration set.) */
    bool c4db_mayHaveExpiration(C4Database *db C4NONNULL) C4API;

    /** Returns the timestamp at which the next document expiration should take place,
        or 0 if there are no documents with expiration times. */
    C4Timestamp c4db_nextDocExpiration(C4Database *database C4NONNULL) C4API;

    /** Purges all documents that have expired.
        @return  The number of documents purged, or -1 on error. */
    int64_t c4db_purgeExpiredDocs(C4Database *db C4NONNULL, C4Error*) C4API;

    /** Starts a background task that automatically purges expired documents.
        @return  True if the task started, false if it couldn't (i.e. database is read-only.) */
    bool c4db_startHousekeeping(C4Database *db C4NONNULL) C4API;

    /** Returns the number of revisions of a document that are tracked. (Defaults to 20.) */
    uint32_t c4db_getMaxRevTreeDepth(C4Database *database C4NONNULL) C4API;

    /** Configures the number of revisions of a document that are tracked. */
    void c4db_setMaxRevTreeDepth(C4Database *database C4NONNULL, uint32_t maxRevTreeDepth) C4API;

    typedef struct C4UUID {
        uint8_t bytes[16];
    } C4UUID;

    /** Returns the database's public and/or private UUIDs. (Pass NULL for ones you don't want.) */
    bool c4db_getUUIDs(C4Database* database C4NONNULL,
                       C4UUID *publicUUID, C4UUID *privateUUID,
                       C4Error *outError) C4API;

    C4ExtraInfo c4db_getExtraInfo(C4Database *database C4NONNULL) C4API;
    void c4db_setExtraInfo(C4Database *database C4NONNULL, C4ExtraInfo) C4API;


    /** @} */
    /** \name Database Maintenance
        @{ */


    /** Types of maintenance that \ref c4db_maintenance can perform.
        NOTE: Enum values must match the ones in DataFile::MaintenanceType */
    typedef C4_ENUM(uint32_t, C4MaintenanceType) {
        kC4Compact,         ///< Compact the database file and garbage-collect attachments
        kC4Reindex,         ///< Rebuild indexes (not normally needed)
        kC4IntegrityCheck,  ///< Check for database corruption, returning an error if it finds any
    };


    /** Performs database maintenance. */
    bool c4db_maintenance(C4Database* database C4NONNULL,
                          C4MaintenanceType type,
                          C4Error *outError) C4API;


    // DEPRECATED -- call c4db_maintenance instead
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
    struct C4RawDocument {
        C4String key;    ///< The key (document ID)
        C4String meta;   ///< Metadata (usage is up to the caller)
        C4String body;   ///< Body data
    };

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
