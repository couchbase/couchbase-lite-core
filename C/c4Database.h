//
//  c4Database.h
//  CBForest
//
//  C API for database and document access.
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Database_h
#define c4Database_h

#include "c4Base.h"

#ifdef __cplusplus
extern "C" {
#endif

    //////// DATABASES:


    /** Boolean options specified when opening a database or view. */
    typedef C4_OPTIONS(uint32_t, C4DatabaseFlags) {
        kC4DB_Create        = 1,    /**< Create the file if it doesn't exist */
        kC4DB_ReadOnly      = 2,    /**< Open file read-only */
        kC4DB_AutoCompact   = 4,    /**< Enable auto-compaction */
    };

    /** Encryption algorithms. */
    typedef C4_ENUM(uint32_t, C4EncryptionAlgorithm) {
        kC4EncryptionNone = 0,      /**< No encryption (default) */
        kC4EncryptionAES256 = 1     /**< AES with 256-bit key */
    };

    typedef struct {
        C4EncryptionAlgorithm algorithm;
        uint8_t bytes[32];
    } C4EncryptionKey;


    /** Opaque handle to an opened database. */
    typedef struct c4Database C4Database;

    /** Opens a database. */
    C4Database* c4db_open(C4Slice path,
                          C4DatabaseFlags flags,
                          const C4EncryptionKey *encryptionKey,
                          C4Error *outError);

    /** Frees a database handle, closing the database first if it's still open. */
    bool c4db_free(C4Database* database);

    /** Closes the database. Does not free the handle, although any operation other than
        c4db_free() will fail with an error. */
    bool c4db_close(C4Database* database, C4Error *outError);

    /** Closes the database, deletes the file, and frees the object. */
    bool c4db_delete(C4Database* database, C4Error *outError);

    /** Deletes the file(s) for the database at the given path.
        All C4Databases at that path should be closed first. */
    bool c4db_deleteAtPath(C4Slice dbPath, C4DatabaseFlags flags, C4Error *outError);

    /** Manually compacts the database. */
    bool c4db_compact(C4Database* database, C4Error *outError);

    /** Returns true if the database is compacting.
        If NULL is passed, returns true if _any_ database is compacting. */
    bool c4db_isCompacting(C4Database*);

    typedef void (*C4OnCompactCallback)(void *context, bool compacting);

    /** Registers a callback to be invoked when the database starts or finishes compacting.
        The callback is likely to be called on a background thread owned by ForestDB, so be
        careful of thread safety. */
    void c4db_setOnCompactCallback(C4Database *database, C4OnCompactCallback cb, void *context);

    /** Changes a database's encryption key (removing encryption if it's NULL.) */
    bool c4db_rekey(C4Database* database,
                    const C4EncryptionKey *newKey,
                    C4Error *outError);

    /** Returns the path of the database. */
    C4SliceResult c4db_getPath(C4Database*);

    /** Returns the number of (undeleted) documents in the database. */
    uint64_t c4db_getDocumentCount(C4Database* database);

    /** Returns the latest sequence number allocated to a revision. */
    C4SequenceNumber c4db_getLastSequence(C4Database* database);

    /** Begins a transaction.
        Transactions can nest; only the first call actually creates a ForestDB transaction. */
    bool c4db_beginTransaction(C4Database* database,
                               C4Error *outError);

    /** Commits or aborts a transaction. If there have been multiple calls to beginTransaction, it takes the same number of calls to endTransaction to actually end the transaction; only the last one commits or aborts the ForestDB transaction. */
    bool c4db_endTransaction(C4Database* database,
                             bool commit,
                             C4Error *outError);

    /** Is a transaction active? */
    bool c4db_isInTransaction(C4Database* database);

    
    /** Removes all trace of a document and its revisions from the database. */
    bool c4db_purgeDoc(C4Database *db, C4Slice docID, C4Error *outError);

    /** Returns the timestamp at which the next document expiration should take place. */
    uint64_t c4db_nextDocExpiration(C4Database *database);

    /** Closes down ForestDB state by calling fdb_shutdown(). */
    bool c4_shutdown(C4Error *outError);

    //////// RAW DOCUMENTS (i.e. info or _local)


    /** Describes a raw document. */
    typedef struct {
        C4Slice key;
        C4Slice meta;
        C4Slice body;
    } C4RawDocument;

    /** Frees the storage occupied by a raw document. */
    void c4raw_free(C4RawDocument* rawDoc);

    /** Reads a raw document from the database. In Couchbase Lite the store named "info" is used for per-database key/value pairs, and the store "_local" is used for local documents. */
    C4RawDocument* c4raw_get(C4Database* database,
                             C4Slice storeName,
                             C4Slice docID,
                             C4Error *outError);

    /** Writes a raw document to the database, or deletes it if both meta and body are NULL. */
    bool c4raw_put(C4Database* database,
                   C4Slice storeName,
                   C4Slice key,
                   C4Slice meta,
                   C4Slice body,
                   C4Error *outError);

    // Store used for database metadata.
    #define kC4InfoStore ((C4Slice){"info", 4})

    // Store used for local (non-replicated) documents.
    #define kC4LocalDocStore ((C4Slice){"_local", 6})

#ifdef __cplusplus
}
#endif

#endif /* c4Database_h */
