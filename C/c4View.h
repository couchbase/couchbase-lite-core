//
//  c4View.h
//  CBForest
//
//  C API for view and query access.
//
//  Created by Jens Alfke on 9/10/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4View_h
#define c4View_h

#include "c4.h"
#include "c4Database.h"
#include "c4Key.h"

#ifdef __cplusplus
extern "C" {
#endif

    //////// VIEWS:


    /** Opaque handle to an opened view. */
    typedef struct c4View C4View;

    /** Opens a view, or creates it if the file doesn't already exist.
        @param database  The database the view is associated with.
        @param path  The filesystem path to the view index file.
        @param viewName  The name of the view.
        @param version  The version of the views map function.
        @param outError  On failure, error info will be stored here.
        @return  The new C4View, or NULL on failure. */
    C4View* c4view_open(C4Database *database,
                        C4Slice path,
                        C4Slice viewName,
                        C4Slice version,
                        C4DatabaseFlags flags,
                        const C4EncryptionKey *encryptionKey,
                        C4Error *outError);

    /** Closes the view and frees the object. */
    bool c4view_close(C4View* view, C4Error*);

    /** Changes a view's encryption key (removing encryption if it's NULL.) */
    bool c4view_rekey(C4View*,
                      const C4EncryptionKey *newKey,
                      C4Error *outError);

    /** Erases the view index, but doesn't delete the database file. */
    bool c4view_eraseIndex(C4View*, C4Error *outError);

    /** Deletes the database file and closes/frees the C4View. */
    bool c4view_delete(C4View*, C4Error *outError);


    /** Returns the total number of rows in the view index. */
    uint64_t c4view_getTotalRows(C4View*);

    /** Returns the last database sequence number that's been indexed.
        If this is less than the database's lastSequence, the view index is out of date. */
    C4SequenceNumber c4view_getLastSequenceIndexed(C4View*);

    /** Returns the last database sequence number that changed the view index. */
    C4SequenceNumber c4view_getLastSequenceChangedAt(C4View*);


    //////// INDEXING:


    /** Opaque reference to an indexing task. */
    typedef struct c4Indexer C4Indexer;

    /** Creates an indexing task on one or more views in a database.
        @param db  The database to index.
        @param views  An array of views whose indexes should be updated in parallel.
        @param viewCount  The number of views in the views[] array.
        @param outError  On failure, error info will be stored here.
        @return  A new C4Indexer, or NULL on failure. */
    C4Indexer* c4indexer_begin(C4Database *db,
                               C4View *views[],
                               size_t viewCount,
                               C4Error *outError);

    /** Creates an enumerator that will return all the documents that need to be (re)indexed. */
    C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer,
                                                  C4Error *outError);

    /** Adds index rows for the keys/values derived from one document, for one view.
        This function needs to be called *exactly once* for each (document, view) pair during
        indexing. (Even if the view's map function didn't emit anything, the old keys/values need to
        be cleaned up.)
     
        Values are uninterpreted by CBForest, but by convention are JSON. A special value "*"
        (a single asterisk) is used as a placeholder for the entire document.

        @param indexer  The indexer task.
        @param document  The document being indexed.
        @param viewNumber  The position of the view in the indexer's views[] array.
        @param emitCount  The number of emitted key/value pairs.
        @param emittedKeys  Array of keys being emitted.
        @param emittedValues  Array of values being emitted. (JSON by convention.)
        @param outError  On failure, error info will be stored here.
        @return  True on success, false on failure. */
    bool c4indexer_emit(C4Indexer *indexer,
                        struct C4Document *document,
                        unsigned viewNumber,
                        unsigned emitCount,
                        C4Key* emittedKeys[],
                        C4Slice emittedValues[],
                        C4Error *outError);

    /** Finishes an indexing task and frees the indexer reference.
        @param indexer  The indexer.
        @param commit  True to commit changes to the indexes, false to abort.
        @param outError  On failure, error info will be stored here.
        @return  True on success, false on failure. */
    bool c4indexer_end(C4Indexer *indexer,
                       bool commit,
                       C4Error *outError);

// A view value that represents a placeholder for the entire document
#ifdef _MSC_VER
#define kC4PlaceholderValue ({"*", 1})
#else
#define kC4PlaceholderValue ((C4Slice){"*", 1})
#endif

    //////// QUERYING:


    /** Options for view queries. */
    typedef struct {
        uint64_t skip;
        uint64_t limit;
        bool descending;
        bool inclusiveStart;
        bool inclusiveEnd;

        C4Key *startKey;
        C4Key *endKey;
        C4Slice startKeyDocID;
        C4Slice endKeyDocID;
        
        const C4Key **keys;
        size_t keysCount;
    } C4QueryOptions;

    /** Default query options. */
	CBFOREST_API extern const C4QueryOptions kC4DefaultQueryOptions;

    /** A view query result enumerator. Created by c4view_query.
        The fields of the object are invalidated by the next call to c4queryenum_next or
        c4queryenum_free. */
    typedef struct {
        C4KeyReader key;
        C4Slice value;
        C4Slice docID;
        C4SequenceNumber docSequence;
    } C4QueryEnumerator;

    /** Runs a query and returns an enumerator for the results.
        The enumerator's fields are not valid until you call c4queryenum_next(), though. */
    C4QueryEnumerator* c4view_query(C4View*,
                                    const C4QueryOptions *options,
                                    C4Error *outError);

    /** Advances a query enumerator to the next row, populating its fields.
        Returns true on success, false at the end of enumeration or on error. */
    bool c4queryenum_next(C4QueryEnumerator *e,
                          C4Error *outError);

    /** Frees a query enumerator. */
    void c4queryenum_free(C4QueryEnumerator *e);

#ifdef __cplusplus
}
#endif

#endif /* c4View_h */
