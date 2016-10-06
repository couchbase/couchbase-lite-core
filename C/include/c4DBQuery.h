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

    /** \defgroup DBQueries Database Queries
        @{ */


    //////// DATABASE QUERIES:


    /** Opaque handle to a compiled query. */
    typedef struct c4Query C4Query;


    /** Compiles a query from an expression given as JSON. */
    C4Query* c4query_new(C4Database *database,
                         C4Slice queryExpression,
                         C4Slice sortExpression,
                         C4Error *error);

    /** Frees a query. */
    void c4query_free(C4Query*);

    /** Runs a compiled query. 
        @param query  The compiled query to run.
        @param options  Query options; only `skip` and `limit` are currently recognized.
        @param encodedParameters  JSON object whose keys correspond to the parameters left
                blank in the query expression, and values correspond to the values to use.
        @param outError  On failure, will be set to the error status.
        @return  An enumerator for reading the rows, or NULL on error. */
    C4QueryEnumerator* c4query_run(C4Query *query,
                                   const C4QueryOptions *options,
                                   C4Slice encodedParameters,
                                   C4Error *outError);

    /** @} */


    //////// INDEXES:


    /** \name Database Indexes
     @{ */


    /** Creates an index on a document property, to speed up subsequent compiled queries.
        @param database  The database to index.
        @param expression  The property to index: a path expression such as "address.street"
                            or "coords[0]".
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_createIndex(C4Database *database,
                          C4Slice expression,
                          C4Error *outError);

    /** Deletes an index that was created by `c4db_createIndex`.
        @param database  The database to index.
        @param expression  The path expression used when creating the index.
        @return  True on success, false on failure. */
    bool c4db_deleteIndex(C4Database *database,
                          C4Slice expression,
                          C4Error *outError);

    /** @} */

#ifdef __cplusplus
}
#endif
