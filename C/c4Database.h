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

#include "c4.h"

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

    /** Closes the database and frees the object. */
    bool c4db_close(C4Database* database, C4Error *outError);

    /** Closes the database, deletes the file, and frees the object. */
    bool c4db_delete(C4Database* database, C4Error *outError);

    /** Manually compacts the database. */
    bool c4db_compact(C4Database* database, C4Error *outError);

    /** Changes a database's encryption key (removing encryption if it's NULL.) */
    bool c4db_rekey(C4Database* database,
                    const C4EncryptionKey *newKey,
                    C4Error *outError);

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
    bool c4db_purgeDoc(struct c4Database *db, C4Slice docID, C4Error *outError);


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


    //////// DATABASE ENUMERATION (BY DOCUMENT):


    typedef C4_OPTIONS(uint16_t, C4EnumeratorFlags) {
        kC4Descending           = 0x01, /**< If true, iteration goes by descending document IDs. */
        kC4InclusiveStart       = 0x02, /**< If false, iteration starts just _after_ startDocID. */
        kC4InclusiveEnd         = 0x04, /**< If false, iteration stops just _before_ endDocID. */
        kC4IncludeDeleted       = 0x08, /**< If true, include deleted documents. */
        kC4IncludeNonConflicted = 0x10, /**< If false, include _only_ documents in conflict. */
        kC4IncludeBodies        = 0x20  /**< If false, document bodies will not be preloaded, just
                                   metadata (docID, revID, sequence, flags.) This is faster if you
                                   don't need to access the revision tree or revision bodies. You
                                   can still access all the data of the document, but it will
                                   trigger loading the document body from the database. */
    };


    /** Options for enumerating over all documents. */
    typedef struct {
        uint64_t          skip;     /**< The number of initial results to skip. */
        C4EnumeratorFlags flags;    /**< Option flags */
    } C4EnumeratorOptions;

    /** Default all-docs enumeration options.
        Includes inclusiveStart, inclusiveEnd, includeBodies, includeNonConflicts.
        Does not include descending, skip, includeDeleted. */
    CBFOREST_API extern const C4EnumeratorOptions kC4DefaultEnumeratorOptions;
    

    /** Opaque handle to a document enumerator. */
    typedef struct C4DocEnumerator C4DocEnumerator;

    /** Frees a C4DocEnumerator handle. */
    void c4enum_free(C4DocEnumerator *e);

    /** Creates an enumerator ordered by sequence.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param since  The sequence number to start _after_. Pass 0 to start from the beginning.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                           C4SequenceNumber since,
                                           const C4EnumeratorOptions *options,
                                           C4Error *outError);

    /** Creates an enumerator ordered by docID.
        Options have the same meanings as in Couchbase Lite.
        There's no 'limit' option; just stop enumerating when you're done.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param startDocID  The document ID to begin at.
        @param endDocID  The document ID to end at.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                           C4Slice startDocID,
                                           C4Slice endDocID,
                                           const C4EnumeratorOptions *options,
                                           C4Error *outError);

    /** Creates an enumerator on a series of document IDs.
        Options have the same meanings as in Couchbase Lite.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param docIDs  Array of doc IDs to traverse in order.
        @param docIDsCount  Number of doc IDs.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateSomeDocs(C4Database *database,
                                            C4Slice docIDs[],
                                            size_t docIDsCount,
                                            const C4EnumeratorOptions *options,
                                            C4Error *outError);

    /** Returns the next document from an enumerator, or NULL if there are no more.
        The caller is responsible for freeing the C4Document.
        Don't forget to free the enumerator itself when finished with it. */
    struct C4Document* c4enum_nextDocument(C4DocEnumerator *e,
                                           C4Error *outError);


#ifdef __cplusplus
}
#endif

#endif /* c4Database_h */
