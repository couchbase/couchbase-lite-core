//
//  c4Query.h
//  LiteCore
//
//  Created by Jens Alfke on 10/5/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "c4View.h"

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup QueryingDB Querying the Database
        @{ */


    //////// DATABASE QUERIES:


    /** Opaque handle to a compiled query. */
    typedef struct c4Query C4Query;


    /** Compiles a query from an expression given as JSON. (The current syntax is based
        on Cloudant's, but this is a placeholder and will be changing.)
        The expression is a predicate that describes which documents should be returned.
        A separate, optional sort expression describes the ordering of the results.

        NOTE: Queries are only supported on SQLite-based databases.
        Queries are currently not supported on databases whose documents use revision trees.
        @param database  The database to be queried.
        @param queryExpression  JSON data describing a predicate that documents must match.
                    The syntax is too complex to describe here; see full documentation elsewhere.
        @param sortExpression  Optional JSON array of strings, describing the property path(s)
                    to sort by. The first path is the primary sort, the second secondary, etc.
                    A path can be prefixed with "-" to denote descending order, or "+" (a no-op.)

                    The special path "_id" denotes the document ID, and "_sequence" denotes the
                    sequence number.

                    An empty array suppresses sorting; this is slightly faster, but the order of
                    results is _undefined_ and should be assumed to be randomized.

                    As a convenience, the JSON may be a single string, which is treated as though
                    it were a one-element array. (Of course, it's still JSON, so it must still
                    be enclosed in double-quotes and escaped.)
     
                    If this parameter is left null, results are ordered by document ID.
        @param error  Error will be written here if the function fails.
        @result  A new C4Query, or NULL on failure. */
    C4Query* c4query_new(C4Database *database,
                         C4Slice queryExpression,
                         C4Slice sortExpression,
                         C4Error *error) C4API;

    /** Frees a query.  It is legal to pass NULL. */
    void c4query_free(C4Query*) C4API;

    /** Runs a compiled query.
        NOTE: Queries will run much faster if the appropriate properties are indexed.
        Indexes must be created explicitly by calling `c4db_createIndex`.
        @param query  The compiled query to run.
        @param options  Query options; only `skip` and `limit` are currently recognized.
        @param encodedParameters  Optional JSON object whose keys correspond to the named
                parameters in the query expression, and values correspond to the values to
                bind. Any unbound parameters will be `null`.
        @param outError  On failure, will be set to the error status.
        @return  An enumerator for reading the rows, or NULL on error. */
    C4QueryEnumerator* c4query_run(C4Query *query,
                                   const C4QueryOptions *options,
                                   C4Slice encodedParameters,
                                   C4Error *outError) C4API;

    /** Given a docID and sequence number from the enumerator, returns the text that was emitted
        during indexing. */
    C4SliceResult c4query_fullTextMatched(C4Query *query,
                                          C4Slice docID,
                                          C4SequenceNumber seq,
                                          C4Error *outError) C4API;
    /** @} */


    //////// INDEXES:


    /** \name Database Indexes
     @{ */


    typedef C4_ENUM(uint32_t, C4IndexType) {
        kC4ValueIndex,         ///< Regular index of property value
        kC4FullTextIndex,      ///< Full-text index
        kC4GeoIndex,           ///< Geospatial index of GeoJSON values
    };


    /** Creates an index on a document property, to speed up subsequent queries.
        Nested properties can be indexed, but their values must be scalar (string, number, bool.)
        It's fine if not every document in the database has such a property.
        @param database  The database to index.
        @param propertyPath  The property to index: a path expression such as "address.street"
                            or "coords[0]".
        @param indexType  The type of index (regular, full-text or geospatial.)
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_createIndex(C4Database *database,
                          C4Slice propertyPath,
                          C4IndexType indexType,
                          C4Error *outError) C4API;

    /** Deletes an index that was created by `c4db_createIndex`.
        @param database  The database to index.
        @param propertyPath  The property path used when creating the index.
        @param indexType  The type of the index.
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_deleteIndex(C4Database *database,
                          C4Slice propertyPath,
                          C4IndexType indexType,
                          C4Error *outError) C4API;

    /** @} */

#ifdef __cplusplus
}
#endif
