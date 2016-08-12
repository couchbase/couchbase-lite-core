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

#include "c4Base.h"
#include "c4Database.h"
#include "c4Document.h"
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
        @param version  The version of the view's map function.
        @param outError  On failure, error info will be stored here.
        @return  The new C4View, or NULL on failure. */
    C4View* c4view_open(C4Database *database,
                        C4Slice path,
                        C4Slice viewName,
                        C4Slice version,
                        C4DatabaseFlags flags,
                        const C4EncryptionKey *encryptionKey,
                        C4Error *outError);

    /** Frees a view handle, closing it if necessary. */
    void c4view_free(C4View* view);

    /** Closes the view. Does not free the handle, but calls to it will return errors. */
    bool c4view_close(C4View* view, C4Error*);

    /** Changes a view's encryption key (removing encryption if it's NULL.) */
    bool c4view_rekey(C4View*,
                      const C4EncryptionKey *newKey,
                      C4Error *outError);

    /** Erases the view index, but doesn't delete the database file. */
    bool c4view_eraseIndex(C4View*, C4Error *outError);

    /** Deletes the view's file(s) and closes/frees the C4View. */
    bool c4view_delete(C4View*, C4Error *outError);

    /** Deletes the file(s) for the view at the given path.
        All C4Databases at that path should be closed first. */
    bool c4view_deleteAtPath(C4Slice dbPath, C4DatabaseFlags flags, C4Error *outError);

    /** Sets the persistent version string associated with the map function. If the new value is
        different from the one previously stored, the index is invalidated. */
    void c4view_setMapVersion(C4View *view, C4Slice version);

    /** Returns the total number of rows in the view index. */
    uint64_t c4view_getTotalRows(C4View*);

    /** Returns the last database sequence number that's been indexed.
        If this is less than the database's lastSequence, the view index is out of date. */
    C4SequenceNumber c4view_getLastSequenceIndexed(C4View*);

    /** Returns the last database sequence number that changed the view index. */
    C4SequenceNumber c4view_getLastSequenceChangedAt(C4View*);


    /** Sets a documentType filter on the view. If non-null, only documents whose
        documentType matches will be indexed by this view. */
    void c4view_setDocumentType(C4View*, C4Slice docType);

    /** Registers a callback to be invoked when the view's index db starts or finishes compacting.
        The callback is likely to be called on a background thread owned by ForestDB, so be
        careful of thread safety. */
    void c4view_setOnCompactCallback(C4View*, C4OnCompactCallback, void *context);


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

    /** Instructs the indexer not to do any indexing if the given view is up-to-date.
        Typically this is used when the indexing occurs because this view is being queried. */
    void c4indexer_triggerOnView(C4Indexer *indexer, C4View *view);

    /** Creates an enumerator that will return all the documents that need to be (re)indexed.
        Returns NULL if no indexing is needed; you can distinguish this from an error by looking
        at the C4Error. */
    struct C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer,
                                                         C4Error *outError);

    /** Returns true if a view being indexed should index the given document.
        (This checks whether the document's current revision's sequence is greater than
        the view's last-indexed sequence.)
        If only one view is being indexed, you don't need to call this (assume it returns true.)
        If this function returns true, the caller should proceed to compute the key/value pairs,
        then call c4indexer_emit() to add them to this view's index.
        If this function returns false, the caller should skip to the next view.*/
    bool c4indexer_shouldIndexDocument(C4Indexer *indexer,
                                       unsigned viewNumber,
                                       C4Document *doc);

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
                        C4Document *document,
                        unsigned viewNumber,
                        unsigned emitCount,
                        C4Key* const emittedKeys[],
                        C4Slice const emittedValues[],
                        C4Error *outError);

    /** Alternate form of c4indexer_emit that takes a C4KeyValueList instead of C arrays. */
    bool c4indexer_emitList(C4Indexer *indexer,
                            C4Document *doc,
                            unsigned viewNumber,
                            C4KeyValueList *kv,
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
        bool rankFullText;

        C4Key *startKey;
        C4Key *endKey;
        C4Slice startKeyDocID;
        C4Slice endKeyDocID;
        
        const C4Key **keys;
        size_t keysCount;
    } C4QueryOptions;

    /** Default query options. */
	CBFOREST_API extern const C4QueryOptions kC4DefaultQueryOptions;

    /** Info about a match of a full-text query term */
    typedef struct {
        uint32_t termIndex;                 ///< Index of the search term in the tokenized query
        uint32_t start, length;             ///< *Byte* range of word in query string
    } C4FullTextTerm;

    /** A view query result enumerator. Created by c4view_query, c4view_fullTextQuery, or
        c4view_geoQuery. Must be freed with c4queryenum_free.
        The fields of this struct represent the current matched index row, and are replaced by the
        next call to c4queryenum_next or c4queryenum_free.
        The memory pointed to by slice fields is valid until the enumerator is advanced or freed. */
    typedef struct {
        // All query types:
        C4Slice docID;                              ///< ID of doc that emitted this row
        C4SequenceNumber docSequence;               ///< Sequence number of doc that emitted row
        C4Slice value;                              ///< Encoded emitted value

        // Map/reduce only:
        C4KeyReader key;                            ///< Encoded emitted key

        // Full-text only:
        unsigned fullTextID;                        ///< cookie for getting the full text string
        uint32_t fullTextTermCount;                 ///< The number of terms that were matched
        const C4FullTextTerm *fullTextTerms;        ///< Array of terms that were matched

        // Geo-query only:
        C4GeoArea geoBBox;                          ///< Bounding box of emitted geoJSON shape
        C4Slice geoJSON;                            ///< GeoJSON description of the shape
    } C4QueryEnumerator;


    /** Runs a regular map/reduce query and returns an enumerator for the results.
        The enumerator's fields are not valid until you call c4queryenum_next(), though.
        @param view  The view to query.
        @param options  Query options, or NULL for the default options.
        @param outError  On failure, error info will be stored here.
        @return  A new query enumerator. Fields are invalid until c4queryenum_next is called. */
    C4QueryEnumerator* c4view_query(C4View *view,
                                    const C4QueryOptions *options,
                                    C4Error *outError);

    /** Runs a full-text query and returns an enumerator for the results.
        @param view  The view to query.
        @param queryString  A string containing the words to search for, separated by whitespace.
        @param queryStringLanguage  The human language of the query string as an ISO-639 code like
                    "en"; or kC4LanguageNone to disable language-specific transformations like
                    stemming; or kC4LanguageDefault to fall back to the default language (as set by
                    c4key_setDefaultFullTextLanguage.)
        @param c4options  Query options. Only skip, limit, descending, rankFullText are used.
        @param outError  On failure, error info will be stored here.
        @return  A new query enumerator. Fields are invalid until c4queryenum_next is called. */
    C4QueryEnumerator* c4view_fullTextQuery(C4View *view,
                                            C4Slice queryString,
                                            C4Slice queryStringLanguage,
                                            const C4QueryOptions *c4options,
                                            C4Error *outError);

    /** Runs a geo-query and returns an enumerator for the results.
        @param view  The view to query.
        @param area  The bounding box to search for. Rows intersecting this will be returned.
        @param outError  On failure, error info will be stored here.
        @return  A new query enumerator. Fields are invalid until c4queryenum_next is called. */
    C4QueryEnumerator* c4view_geoQuery(C4View *view,
                                       C4GeoArea area,
                                       C4Error *outError);

    /** In a full-text query enumerator, returns the string that was emitted during indexing that
        contained the search term(s). */
    C4SliceResult c4queryenum_fullTextMatched(C4QueryEnumerator *e);

    /** Given a document and the fullTextID from the enumerator, returns the text that was emitted
        during indexing. */
    C4SliceResult c4view_fullTextMatched(C4View *view,
                                         C4Slice docID,
                                         C4SequenceNumber seq,
                                         unsigned fullTextID,
                                         C4Error *outError);

    /** Advances a query enumerator to the next row, populating its fields.
        Returns true on success, false at the end of enumeration or on error. */
    bool c4queryenum_next(C4QueryEnumerator *e,
                          C4Error *outError);

    /** Closes an enumerator without freeing it. This is optional, but can be used to free up
        resources if the enumeration has not reached its end, but will not be freed for a while. */
    void c4queryenum_close(C4QueryEnumerator *e);

    /** Frees a query enumerator. */
    void c4queryenum_free(C4QueryEnumerator *e);

#ifdef __cplusplus
}
#endif

#endif /* c4View_h */
