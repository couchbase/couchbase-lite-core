//
// c4Index.h
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4IndexTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Indexing  Database Indexes
     @{ */


    /** Creates a database index, of the values of specific expressions across all documents.
        The name is used to identify the index for later updating or deletion; if an index with the
        same name already exists, it will be replaced unless it has the exact same expressions.

        Currently four types of indexes are supported:

        * Value indexes speed up queries by making it possible to look up property (or expression)
          values without scanning every document. They're just like regular indexes in SQL or N1QL.
          Multiple expressions are supported; the first is the primary key, second is secondary.
          Expressions must evaluate to scalar types (boolean, number, string).
        * Full-Text Search (FTS) indexes enable fast search of natural-language words or phrases
          by using the `MATCH` operator in a query. A FTS index is **required** for full-text
          search: a query with a `MATCH` operator will fail to compile unless there is already a
          FTS index for the property/expression being matched. Only a single expression is
          currently allowed, and it must evaluate to a string.
        * Array indexes optimize UNNEST queries, by materializing an unnested array property
          (across all documents) as a table in the SQLite database, and creating a SQL index on it.
        * Predictive indexes optimize queries that use the PREDICTION() function, by materializing
          the function's results as a table and creating a SQL index on a result property.

        Note: If some documents are missing the values to be indexed,
        those documents will just be omitted from the index. It's not an error.

        In an array index, the first expression must evaluate to an array to be unnested; it's
        usually a property path but could be some other expression type. If the array items are
        nonscalar (dictionaries or arrays), you should add a second expression defining the sub-
        property (or computed value) to index, relative to the array item.

        In a predictive index, the expression is a PREDICTION() call in JSON query syntax,
        including the optional 3rd parameter that gives the result property to extract (and index.)

        `indexSpecJSON` specifies the index as a JSON object, with properties:
        * `WHAT`: An array of expressions in the JSON query syntax. (Note that each
          expression is already an array, so there are two levels of nesting.)
        * `WHERE`: An optional expression. Including this creates a _partial index_: documents
          for which this expression returns `false` or `null` will be skipped.

        For backwards compatibility, `indexSpecJSON` may be an array; this is treated as if it were
        a dictionary with a `WHAT` key mapping to that array.

        Expressions are defined in JSON, as in a query, and wrapped in a JSON array. For example,
        `[[".name.first"]]` will index on the first-name property. Note the two levels of brackets,
        since an expression is already an array.

        @param database  The database to index.
        @param name  The name of the index. Any existing index with the same name will be replaced,
                     unless it has the identical expressions (in which case this is a no-op.)
        @param indexSpecJSON  The definition of the index in JSON form. (See above.)
        @param indexType  The type of index (value or full-text.)
        @param indexOptions  Options for the index. If NULL, each option will get a default value.
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_createIndex(C4Database *database,
                          C4String name,
                          C4String indexSpecJSON,
                          C4IndexType indexType,
                          const C4IndexOptions* C4NULLABLE indexOptions,
                          C4Error* C4NULLABLE outError) C4API;
        
    /** @param database  The database to index.
        @param name  The name of the index. Any existing index with the same name will be replaced,
                     unless it has the identical expressions (in which case this is a no-op.)
        @param indexSpec  The definition of the index in JSON or N1QL form. (See above.)
        @param queryLanguage The query language (JSON or N1QL) of `indexSpec` is expressed.
        @param indexType  The type of index (value or full-text.)
        @param indexOptions  Options for the index. If NULL, each option will get a default value.
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_createIndex2(C4Database *database,
                        C4String name,
                        C4String indexSpec,
                        C4QueryLanguage queryLanguage,
                        C4IndexType indexType,
                        const C4IndexOptions* C4NULLABLE indexOptions,
                        C4Error* C4NULLABLE outError) C4API;


    /** Deletes an index that was created by `c4db_createIndex`.
        @param database  The database to index.
        @param name The name of the index to delete
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_deleteIndex(C4Database *database,
                          C4String name,
                          C4Error* C4NULLABLE outError) C4API;


    /** Returns information about all indexes in the database.
        The result is a Fleece-encoded array of dictionaries, one per index.
        Each dictionary has keys `"name"`, `"type"` (a `C4IndexType`), and `"expr"` (the source expression).
        @param database  The database to check
        @param outError  On failure, will be set to the error status.
        @return  A Fleece-encoded array of dictionaries, or NULL on failure. */
    C4SliceResult c4db_getIndexesInfo(C4Database* database,
                                      C4Error* C4NULLABLE outError) C4API;

    /** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
