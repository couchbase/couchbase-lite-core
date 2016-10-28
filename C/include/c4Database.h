//
//  c4Database.h
//  Couchbase Lite Core
//
//  C API for database and document access.
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
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
        kC4DB_Bundled       = 8,    ///< Store db (and views) inside a directory
        kC4DB_SharedKeys    = 0x10, ///< Enable shared-keys optimization at creation time
    };

    /** Document versioning system (also determines database storage schema) */
    typedef C4_ENUM(uint32_t, C4DocumentVersioning) {
        kC4RevisionTrees,           ///< CouchDB and Couchbase Mobile 1.x revision trees
        kC4VersionVectors,          ///< Couchbase Mobile 2.x version vectors
    };

    /** Encryption algorithms. */
    typedef C4_ENUM(uint32_t, C4EncryptionAlgorithm) {
        kC4EncryptionNone = 0,      ///< No encryption (default)
        kC4EncryptionAES256 = 1     ///< AES with 256-bit key
    };

    /** Encryption key specified in a C4DatabaseConfig. */
    typedef struct C4EncryptionKey {
        C4EncryptionAlgorithm algorithm;
        uint8_t bytes[32];
    } C4EncryptionKey;

    /** Underlying storage engines that can be used. */
    typedef const char* C4StorageEngine;
    extern C4StorageEngine const kC4ForestDBStorageEngine;
    extern C4StorageEngine const kC4SQLiteStorageEngine;

    /** Main database/view configuration struct. */
    typedef struct C4DatabaseConfig {
        C4DatabaseFlags flags;          ///< Create, ReadOnly, AutoCompact, Bundled...
        C4StorageEngine storageEngine;  ///< Which storage to use, or NULL for no preference
        C4DocumentVersioning versioning;///< Type of document versioning
        C4EncryptionKey encryptionKey;  ///< Encryption to use creating/opening the db
    } C4DatabaseConfig;


    /** @} */

    //////// DATABASE API:

    
    /** \name Lifecycle
        @{ */

    /** Opaque handle to an opened database. */
    typedef struct c4Database C4Database;


    /** Opens a database. */
    C4Database* c4db_open(C4Slice path,
                          const C4DatabaseConfig *config,
                          C4Error *outError) C4API;

    /** Frees a database handle, closing the database first if it's still open. */
    bool c4db_free(C4Database* database) C4API;

    /** Closes the database. Does not free the handle, although any operation other than
        c4db_free() will fail with an error. */
    bool c4db_close(C4Database* database, C4Error *outError) C4API;

    /** Closes the database, deletes the file, and frees the object. */
    bool c4db_delete(C4Database* database, C4Error *outError) C4API;

    /** Deletes the file(s) for the database at the given path.
        All C4Databases at that path should be closed first. */
    bool c4db_deleteAtPath(C4Slice dbPath, const C4DatabaseConfig *config, C4Error *outError) C4API;


    /** Changes a database's encryption key (removing encryption if it's NULL.) */
    bool c4db_rekey(C4Database* database,
                    const C4EncryptionKey *newKey,
                    C4Error *outError) C4API;

    /** Closes down the storage engines. Must close all databases first.
        You don't generally need to do this, but it can be useful in tests. */
    bool c4_shutdown(C4Error *outError) C4API;


    /** @} */
    /** \name Accessors
        @{ */


    /** Returns the path of the database. */
    C4SliceResult c4db_getPath(C4Database*) C4API;

    /** Returns the configuration the database was opened with. */
    const C4DatabaseConfig* c4db_getConfig(C4Database*) C4API;

    /** Returns the number of (undeleted) documents in the database. */
    uint64_t c4db_getDocumentCount(C4Database* database) C4API;

    /** Returns the latest sequence number allocated to a revision. */
    C4SequenceNumber c4db_getLastSequence(C4Database* database) C4API;

    /** Returns the timestamp at which the next document expiration should take place. */
    uint64_t c4db_nextDocExpiration(C4Database *database) C4API;


    /** @} */
    /** \name Compaction
        @{ */


    /** Manually compacts the database. */
    bool c4db_compact(C4Database* database, C4Error *outError) C4API;

    /** Returns true if the database is compacting.
        If NULL is passed, returns true if _any_ database is compacting. */
    bool c4db_isCompacting(C4Database*) C4API;

    typedef void (*C4OnCompactCallback)(void *context, bool compacting);

    /** Registers a callback to be invoked when the database starts or finishes compacting.
        The callback is likely to be called on a background thread owned by ForestDB, so be
        careful of thread safety. */
    void c4db_setOnCompactCallback(C4Database *database, C4OnCompactCallback cb, void *context) C4API;


    /** @} */
    /** \name Transactions
        @{ */


    /** Begins a transaction.
        Transactions can nest; only the first call actually creates a database transaction. */
    bool c4db_beginTransaction(C4Database* database,
                               C4Error *outError) C4API;

    /** Commits or aborts a transaction. If there have been multiple calls to beginTransaction, it
        takes the same number of calls to endTransaction to actually end the transaction; only the
        last one commits or aborts the database transaction. */
    bool c4db_endTransaction(C4Database* database,
                             bool commit,
                             C4Error *outError) C4API;

    /** Is a transaction active? */
    bool c4db_isInTransaction(C4Database* database) C4API;

    
    /** @} */
    /** @} */


    //////// RAW DOCUMENTS (i.e. info or _local)


    /** \defgroup RawDocs Raw Documents
        @{ */


    /** Contents of a raw document. */
    typedef struct {
        C4Slice key;    ///< The key (document ID)
        C4Slice meta;   ///< Metadata (usage is up to the caller)
        C4Slice body;   ///< Body data
    } C4RawDocument;

    /** Frees the storage occupied by a raw document. */
    void c4raw_free(C4RawDocument* rawDoc) C4API;

    /** Reads a raw document from the database. In Couchbase Lite the store named "info" is used for per-database key/value pairs, and the store "_local" is used for local documents. */
    C4RawDocument* c4raw_get(C4Database* database,
                             C4Slice storeName,
                             C4Slice docID,
                             C4Error *outError) C4API;

    /** Writes a raw document to the database, or deletes it if both meta and body are NULL. */
    bool c4raw_put(C4Database* database,
                   C4Slice storeName,
                   C4Slice key,
                   C4Slice meta,
                   C4Slice body,
                   C4Error *outError) C4API;

    // Store used for database metadata.
    #define kC4InfoStore C4STR("info")

    // Store used for local (non-replicated) documents.
    #define kC4LocalDocStore C4STR("_local")

    /** @} */
#ifdef __cplusplus
}
#endif
