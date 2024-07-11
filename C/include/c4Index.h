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

//======== C4Collection Methods:

/** \defgroup Indexing  Indexes
     @{ */

/** Creates a collection index, of the values of specific expressions across all documents.
    The name is used to identify the index for later updating or deletion; if an index with the
    same name already exists, it will be replaced unless it has the exact same expressions.

    Currently five types of indexes are supported:

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
    * Vector indexes store high-dimensional vectors/embeddings and support efficient Approximate
      Nearest Neighbor (ANN) queries for finding the nearest vectors to a query vector.

    Note: If some documents are missing the values to be indexed,
    those documents will just be omitted from the index. It's not an error.

    In an array index, the first expression must evaluate to an array to be unnested; it's
    usually a property path but could be some other expression type. If the array items are
    nonscalar (dictionaries or arrays), you should add a second expression defining the sub-
    property (or computed value) to index, relative to the array item.

    In a predictive index, the expression is a PREDICTION() call in JSON query syntax,
    including the optional 3rd parameter that gives the result property to extract (and index.)

    The `indexSpec` argument is an expression, relative to a document, that describes what to index.
    It can be in either the JSON query schema, or in N1QL syntax. It usually names a property,
    but may also be a computed value based on properties.

    @param collection  The collection to index.
    @param name  The name of the index. Any existing index with the same name will be replaced,
                 unless it has the identical expressions (in which case this is a no-op.)
    @param indexSpec  The definition of the index in JSON or N1QL form. (See above.)
    @param queryLanguage  The language of `indexSpec`, either JSON or N1QL.
    @param indexType  The type of index (value full-text, etc.)
    @param indexOptions  Options for the index. If NULL, each option will get a default value.
    @param outError  On failure, will be set to the error status.
    @return  True on success, false on failure. */
NODISCARD CBL_CORE_API bool c4coll_createIndex(C4Collection* collection, C4String name, C4String indexSpec,
                                               C4QueryLanguage queryLanguage, C4IndexType indexType,
                                               const C4IndexOptions* C4NULLABLE indexOptions,
                                               C4Error* C4NULLABLE              outError) C4API;

/** Returns an object representing an existing index. */
CBL_CORE_API C4Index* C4NULLABLE c4coll_getIndex(C4Collection* collection, C4String name,
                                                 C4Error* C4NULLABLE outError) C4API;

/** Deletes an index that was created by `c4coll_createIndex`.
    @param collection  The collection to index.
    @param name The name of the index to delete
    @param outError  On failure, will be set to the error status.
    @return  True on success, false on failure. */
NODISCARD CBL_CORE_API bool c4coll_deleteIndex(C4Collection* collection, C4String name,
                                               C4Error* C4NULLABLE outError) C4API;

/** Returns information about all indexes in the collection.
    The result is a Fleece-encoded array of dictionaries, one per index.
    Each dictionary has keys `"name"`, `"type"` (a `C4IndexType`), and `"expr"` (the source expression).
    @param collection  The collection to check
    @param outError  On failure, will be set to the error status.
    @return  A Fleece-encoded array of dictionaries, or NULL on failure. */
CBL_CORE_API C4SliceResult c4coll_getIndexesInfo(C4Collection* collection, C4Error* C4NULLABLE outError) C4API;

/** Returns information about all indexes in the database.
    The result is a Fleece-encoded array of dictionaries, one per index.
    Each dictionary has keys `"name"`, `"type"` (a `C4IndexType`), and `"expr"` (the source expression).
    @param database  The database to check
    @param outError  On failure, will be set to the error status.
    @return  A Fleece-encoded array of dictionaries, or NULL on failure. */
CBL_CORE_API C4SliceResult c4db_getIndexesInfo(C4Database* database, C4Error* C4NULLABLE outError) C4API;

//======== C4Index Methods:

/** Returns the index's type. */
CBL_CORE_API C4IndexType c4index_getType(C4Index*) C4API;

/** Returns the index's query language (JSON or N1QL). */
CBL_CORE_API C4QueryLanguage c4index_getQueryLanguage(C4Index*) C4API;

/** Returns the indexed expression. */
CBL_CORE_API C4String c4index_getExpression(C4Index*) C4API;

/** Gets the index's FTS/vector options, if any.
    @param index  The index.
    @param outOpts  The options will be written here, if they exist.
    @returns  True if there are options, false if not. */
CBL_CORE_API bool c4index_getOptions(C4Index* index, C4IndexOptions* outOpts) C4API;


#ifdef COUCHBASE_ENTERPRISE

/** Returns whether a vector index has been trained yet or not.
    If the index doesn't exist, or is not a vector index, then this method will
    return false with an appropriate error set.  Otherwise, in the absence of errors,
    this method will zero the error and set the return value. */
CBL_CORE_API bool c4index_isTrained(C4Index*, C4Error* C4NULLABLE outError) C4API;


//======== UPDATING LAZY INDEXES:


/** Finds new or updated documents for which vectors need to be recomputed by the application.
    If there are none, returns NULL.
    If it returns a non-NULL `C4IndexUpdater` object pointer, you should:
    1. Call `valueAt` for each of the `count` items to get the Fleece value, and:
      1.1. Compute a vector from this value
      1.2. Call `setVectorAt` with the resulting vector, or with nullptr if none.
    2. Call `finish` to apply the updates to the index.
    3. Release the `C4IndexUpdater`, of course.

    @note The updater is not guaranteed to find all of the unindexed documents at once! It may
        return less than the limit, even if more exist. It _is_ guaranteed to make progress,
        by returning _some_ unindexed documents if there are any. The intention is that the app
        will continue updating the index periodically until this call returns NULL, signaling
        that the index is now up-to-date.

    @param index  The index to update; must be a vector index with the lazy attribute.
    @param limit  The maximum number of out-of-date documents to include.
    @param outError  On failure, will be set to the error status.
    @return  A new `C4IndexUpdater` reference, or NULL if there's nothing to update. */
NODISCARD CBL_CORE_API C4IndexUpdater* C4NULLABLE c4index_beginUpdate(C4Index* index, size_t limit,
                                                                      C4Error* outError) C4API;

/** Returns the number of vectors to compute. */
CBL_CORE_API size_t c4indexupdater_count(C4IndexUpdater* updater) C4API;

/** Returns the i'th value to compute a vector from.
    This is _not_ the entire document, just the value of the expression in the index spec.
    @param updater  The index updater.
    @param i  The zero-based index of the document.
    @returns  A Fleece value: the value of the index's query expression evaluated on the i'th document.
              Internally this value is part of a query result. It remains valid until the index
              updater is released. If you want to keep it longer, retain it with `FLRetain`. */
NODISCARD CBL_CORE_API FLValue c4indexupdater_valueAt(C4IndexUpdater* updater, size_t i) C4API;

/** Sets the vector for the i'th value. If you don't call this, it's assumed there is no
    vector, and any existing vector will be removed upon `finish`.
    @param updater  The index updater.
    @param i  The zero-based index of the document.
    @param vector  A pointer to the raw vector, or NULL if there is no vector.
    @param dimension  The dimension of `vector`; must be equal to the index's declared dimension.
    @param outError  On failure, will be set to the error status.
    @return  True on success, false on failure. */
NODISCARD CBL_CORE_API bool c4indexupdater_setVectorAt(C4IndexUpdater* updater, size_t i,
                                                       const float vector[C4NULLABLE], size_t dimension,
                                                       C4Error* outError) C4API;

/** Explicitly skips updating the i'th vector. No index entry will be created or deleted.
    The vector still needs to be recomputed, and will be included in the next update request.
    This should be called if there was a transient failure getting the vector, and the app
    will retry later.
    @param updater  The index updater.
    @param i  The zero-based index of the document.
    @return  True on success, false if `i` is out of range. */
CBL_CORE_API bool c4indexupdater_skipVectorAt(C4IndexUpdater* updater, size_t i) C4API;

/** Updates the index with the computed vectors, removes any index rows for which no vector
    was given, and updates the index's latest sequence.

    @note The `C4IndexUpdater` still needs to be released afterwards to prevent a memory leak.
        It's OK to release it without calling this function; the update will effectively be canceled
        and the database left unchanged.

    @param updater  The index updater.
    @param outError  On failure, will be set to the error status.
    @returns  True if the index is now completely up-to-date; false if there are more vectors
              that need to be updated. */
CBL_CORE_API bool c4indexupdater_finish(C4IndexUpdater* updater, C4Error* outError) C4API;

#endif


#ifndef C4_STRICT_COLLECTION_API
//======== SEMI-DEPRECATED DATABASE METHODS:
NODISCARD CBL_CORE_API bool c4db_createIndex(C4Database* database, C4String name, C4String indexSpecJSON,
                                             C4IndexType indexType, const C4IndexOptions* C4NULLABLE indexOptions,
                                             C4Error* C4NULLABLE outError) C4API;
NODISCARD CBL_CORE_API bool c4db_createIndex2(C4Database* database, C4String name, C4String indexSpec,
                                              C4QueryLanguage queryLanguage, C4IndexType indexType,
                                              const C4IndexOptions* C4NULLABLE indexOptions,
                                              C4Error* C4NULLABLE              outError) C4API;
NODISCARD CBL_CORE_API bool c4db_deleteIndex(C4Database* database, C4String name, C4Error* C4NULLABLE outError) C4API;
#endif

CBL_CORE_API bool c4coll_isIndexTrained(C4Collection* collection, C4String name, C4Error* C4NULLABLE outError) C4API;

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
