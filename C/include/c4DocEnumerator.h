//
//  c4DocEnumerator.h
//  CBForest
//
//  Created by Jens Alfke on 12/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4DocEnumerator_h
#define c4DocEnumerator_h

#include "c4Document.h"

#ifdef __cplusplus
extern "C" {
#endif

    //////// DOCUMENT ENUMERATION (ALL_DOCS):


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
    

    /** Metadata about a document (actually about its current revision.) */
    typedef struct {
        C4DocumentFlags flags;      /**< Document flags */
        C4Slice docID;              /**< Document ID */
        C4Slice revID;              /**< RevID of current revision */
        C4SequenceNumber sequence;  /**< Sequence at which doc was last updated */
    } C4DocumentInfo;


    /** Opaque handle to a document enumerator. */
    typedef struct C4DocEnumerator C4DocEnumerator;


    /** Closes an enumeration. This is optional, but can be used to free up resources if the
        enumeration has not reached its end, but will not be freed for a while. */
    void c4enum_close(C4DocEnumerator *e);

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
                                            const C4Slice docIDs[],
                                            size_t docIDsCount,
                                            const C4EnumeratorOptions *options,
                                            C4Error *outError);

    /** Advances the enumerator to the next document.
        Returns false at the end, or on error; look at the C4Error to determine which occurred,
        and don't forget to free the enumerator. */
    bool c4enum_next(C4DocEnumerator *e, C4Error *outError);

    /** Returns the current document, if any, from an enumerator.
        @param e  The enumerator.
        @param outError  Error will be stored here on failure.
        @return  The document, or NULL if there is none or if an error occurred reading its body.
                 Caller is responsible for calling c4document_free when done with it. */
    struct C4Document* c4enum_getDocument(C4DocEnumerator *e,
                                          C4Error *outError);

    /** Stores the metadata of the enumerator's current document into the supplied
        C4DocumentInfo struct. Unlike c4enum_getDocument(), this allocates no memory.
        @param e  The enumerator.
        @param outInfo  A pointer to a C4DocumentInfo struct that will be filled in if a document
                        is found.
        @return  True if the info was stored, false if there is no current document. */
    bool c4enum_getDocumentInfo(C4DocEnumerator *e, C4DocumentInfo *outInfo);

    /** Convenience function that combines c4enum_next() and c4enum_getDocument().
        @param e  The enumerator.
        @param outError  Error will be stored here on failure.
        @return  The next document, or NULL at the end of the enumeration (or an error occurred.)
                 Caller is responsible for calling c4document_free when done with it. */
    struct C4Document* c4enum_nextDocument(C4DocEnumerator *e,
                                           C4Error *outError);

#ifdef __cplusplus
    }
#endif

#endif /* c4DocEnumerator_hh */
