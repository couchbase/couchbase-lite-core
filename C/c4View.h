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

#include "c4Database.h"

#ifdef __cplusplus
extern "C" {
#endif


    //////// KEYS:


    /** An opaque value used as a key in a view index. JSON-compatible. */
    typedef struct c4Key C4Key;

    C4Key* c4key_new();
    void c4key_free(C4Key*);

    void c4Key_addNull(C4Key*);
    void c4key_addBool(C4Key*, bool);
    void c4key_addNumber(C4Key*, double);
    void c4key_addString(C4Key*, C4Slice);

    void c4key_addMapKey(C4Key*, C4Slice);

    void c4key_beginArray(C4Key*);
    void c4key_endArray(C4Key*);
    void c4key_beginMap(C4Key*);
    void c4key_endMap(C4Key*);


    typedef C4Slice C4KeyReader;

    typedef enum {
        kC4Null,
        kC4Bool,
        kC4Number,
        kC4String,
        kC4Array,
        kC4Map,
        kC4EndSequence,
        kC4Special,
        kC4Error = 255
    } C4KeyItemType;

    C4KeyItemType c4key_peek(C4KeyReader*);
    void c4key_next(C4KeyReader*);
    bool c4Key_readBool(C4KeyReader*);
    double c4Key_readNumber(C4KeyReader*);
    C4SliceResult c4Key_readString(C4KeyReader*);  // remember to free the result


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
                        C4Error *outError);

    /** Closes the view and frees the object. */
    bool c4view_close(C4View* view, C4Error*);

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
                               int viewCount,
                               C4Error *outError);

    /** Creates an enumerator that will return all the documents that need to be (re)indexed. */
    C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer,
                                                  C4Error *outError);

    /** Emits new keys/values derived from one document, for one view.
        This function needs to be called once for each (document, view) pair. Even if the view's map
        function didn't emit anything, the old keys/values need to be cleaned up.
        @param indexer  The indexer task.
        @param document  The document being indexed.
        @param viewNumber  The position of the view in the indexer's views[] array.
        @param emitCount  The number of emitted key/value pairs.
        @param emittedKeys  Array of keys being emitted.
        @param emittedValues  Array of values being emitted.
        @param outError  On failure, error info will be stored here.
        @return  True on success, false on failure. */
    bool c4indexer_emit(C4Indexer *indexer,
                        C4Document *document,
                        unsigned viewNumber,
                        unsigned emitCount,
                        C4Key* emittedKeys[],
                        C4Key* emittedValues[],
                        C4Error *outError);

    /** Finishes an indexing task and frees the indexer reference.
        @param indexer  The indexer.
        @param commit  True to commit changes to the indexes, false to abort.
        @param outError  On failure, error info will be stored here.
        @return  True on success, false on failure. */
    bool c4indexer_end(C4Indexer *indexer,
                       bool commit,
                       C4Error *outError);


    //////// QUERYING:


    /** Options for view queries. */
    typedef struct {
        unsigned skip;
        unsigned limit;
        unsigned groupLevel;
        unsigned prefixMatchLevel;
        C4Slice startKeyJSON;
        C4Slice endKeyJSON;
        C4Slice startKeyDocID;
        C4Slice endKeyDocID;
        const C4Slice* keys;
        unsigned keysCount;
        bool descending;
        bool includeDocs;
        bool updateSeq;
        bool localSeq;
        bool inclusiveStart;
        bool inclusiveEnd;
        bool reduceSpecified;
        bool reduce;                   // Ignored if !reduceSpecified
        bool group;
    } C4QueryOptions;

    /** Default query options. */
    extern const C4QueryOptions kC4DefaultQueryOptions;

    /** Opaque reference to a view query result enumerator. */
    typedef struct c4QueryEnumerator C4QueryEnumerator;

    typedef struct {
        C4KeyReader key;
        C4KeyReader value;
        C4Slice docID;
    } C4QueryRow;

    /** Runs a query and returns an enumerator for the results. */
    C4QueryEnumerator* c4view_query(C4View*,
                                    const C4QueryOptions *options,
                                    C4Error *outError);

    /** Returns the next result row from a view query, or NULL at the end of the results. */
    C4QueryRow* c4queryenum_next(C4QueryEnumerator *e);

    /** Frees a query enumerator. Must be called after you're finished with it. */
    void c4queryenum_free(C4QueryEnumerator *e);

    /** Frees a query row. */
    void c4queryrow_free(C4QueryRow *row);

#ifdef __cplusplus
}
#endif

#endif /* c4View_h */
