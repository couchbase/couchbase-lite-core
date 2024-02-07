//
// c4Query.h
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4QueryTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup QueryingDB Querying the Database
        @{ */


//////// DATABASE QUERIES:


/** Compiles a query from an expression given as JSON.
        The expression is a predicate that describes which documents should be returned.
        A separate, optional sort expression describes the ordering of the results.
        @param database  The database to be queried.
        @param language  The language (syntax) of the query expression.
        @param expression  The query expression, either JSON or N1QL.
        @param outErrorPos  If non-NULL, then on a parse error the approximate byte offset in the
                        input expression will be stored here (or -1 if not known/applicable.)
        @param error  Error will be written here if the function fails.
        @result  A new C4Query, or NULL on failure. */
NODISCARD CBL_CORE_API C4Query* C4NULLABLE c4query_new2(C4Database* database, C4QueryLanguage language,
                                                        C4String expression, int* C4NULLABLE outErrorPos,
                                                        C4Error* C4NULLABLE error) C4API;

/** Returns a string describing the implementation of the compiled query.
        This is intended to be read by a developer for purposes of optimizing the query, especially
        to add database indexes. */
CBL_CORE_API C4StringResult c4query_explain(C4Query*) C4API;


/** Returns the number of columns (the values specified in the WHAT clause) in each row. */
CBL_CORE_API unsigned c4query_columnCount(C4Query*) C4API;

/** Returns a suggested title for a column, which may be:
         * An alias specified in an 'AS' modifier in the column definition
         * A property name
         * A function/operator that computes the column value, e.g. 'MAX()' or '+'
        Each column's title is unique. If multiple columns would have the same title, the
        later ones (in numeric order) will have " #2", "#3", etc. appended. */
CBL_CORE_API FLString c4query_columnTitle(C4Query*, unsigned column) C4API;


//////// RUNNING QUERIES:


/** Sets the parameter values to use when running the query, if no parameters are given to
        \ref c4query_run.
        @param query  The compiled query to run.
        @param encodedParameters  JSON- or Fleece-encoded dictionary whose keys correspond
                to the named parameters in the query expression, and values correspond to the
                values to bind. Any unbound parameters will be `null`. */
CBL_CORE_API void c4query_setParameters(C4Query* query, C4String encodedParameters) C4API;


/** Runs a compiled query.
        NOTE: Queries will run much faster if the appropriate properties are indexed.
        Indexes must be created explicitly by calling `c4db_createIndex`.
        @param query  The compiled query to run.
        @param encodedParameters  Options parameter values; if this parameter is not NULL,
                        it overrides the parameters assigned by \ref c4query_setParameters.
        @param outError  On failure, will be set to the error status.
        @return  An enumerator for reading the rows, or NULL on error. */
NODISCARD CBL_CORE_API C4QueryEnumerator* C4NULLABLE c4query_run(C4Query* query, C4String encodedParameters,
                                                                 C4Error* C4NULLABLE outError) C4API;

/** Given a C4FullTextMatch from the enumerator, returns the entire text of the property that
        was matched. (The result depends only on the term's `dataSource` and `property` fields,
        so if you get multiple matches of the same property in the same document, you can skip
        redundant calls with the same values.)
        To find the actual word that was matched, use the term's `start` and `length` fields
        to get a substring of the returned (UTF-8) string. */
CBL_CORE_API C4StringResult c4query_fullTextMatched(C4Query* query, const C4FullTextMatch* term,
                                                    C4Error* C4NULLABLE outError) C4API;

/** Advances a query enumerator to the next row, populating its fields.
        Returns true on success, false at the end of enumeration or on error. */
NODISCARD CBL_CORE_API bool c4queryenum_next(C4QueryEnumerator* e, C4Error* C4NULLABLE outError) C4API;

/** Returns the total number of rows in the query, if known.
        Not all query enumerators may support this (but the current implementation does.)
        @param e  The query enumerator
        @param outError  On failure, an error will be stored here (probably kC4ErrorUnsupported.)
        @return  The number of rows, or -1 on failure. */
NODISCARD CBL_CORE_API int64_t c4queryenum_getRowCount(C4QueryEnumerator* e, C4Error* C4NULLABLE outError) C4API;

/** Jumps to a specific row. Not all query enumerators may support this (but the current
        implementation does.)
        @param e  The query enumerator
        @param rowIndex  The number of the row, starting at 0, or -1 to restart before first row
        @param outError  On failure, an error will be stored here (probably kC4ErrorUnsupported.)
        @return  True on success, false on failure. */
NODISCARD CBL_CORE_API bool c4queryenum_seek(C4QueryEnumerator* e, int64_t rowIndex,
                                             C4Error* C4NULLABLE outError) C4API;

/** Restarts the enumeration, as though it had just been created: the next call to
        \ref c4queryenum_next will read the first row, and so on from there. */
NODISCARD static inline bool c4queryenum_restart(C4QueryEnumerator* e, C4Error* C4NULLABLE outError) C4API {
    return c4queryenum_seek(e, -1, outError);
}

/** Checks whether the query results have changed since this enumerator was created;
        if so, returns a new enumerator. Otherwise returns NULL. */
NODISCARD CBL_CORE_API C4QueryEnumerator* C4NULLABLE c4queryenum_refresh(C4QueryEnumerator*  e,
                                                                         C4Error* C4NULLABLE outError) C4API;

/** Closes an enumerator without freeing it. This is optional, but can be used to free up
        resources if the enumeration has not reached its end, but will not be freed for a while. */
CBL_CORE_API void c4queryenum_close(C4QueryEnumerator*) C4API;

#ifdef COUCHBASE_ENTERPRISE

/** Represents a lazy index. Acts as a factory for C4LazyIndexUpdate objects.
    This is a reference-counted type. */
typedef struct C4LazyIndex C4LazyIndex;

static inline void c4lazyindex_release(C4LazyIndex* C4NULLABLE i) C4API { c4base_release(i); }

/** Describes a set of index values that need to be computed by the application.
    This is a reference-counted type. */
typedef struct C4LazyIndexUpdate C4LazyIndexUpdate;

static inline void c4lazyindexupdate_release(C4LazyIndexUpdate* C4NULLABLE u) C4API { c4base_release(u); }

/** Creates a C4LazyIndex object that can be used to update the index. */
CBL_CORE_API C4LazyIndex* c4lazyindex_open(C4Collection* coll, C4String indexName, C4Error* outError) C4API;

/** Finds new or updated documents for which vectors need to be recomputed by the application.
    If there are none, returns NULL.
    If it returns a non-NULL `C4LazyIndexUpdate` object pointer, you should:
    1. Call `valueAt` for each of the `count` items to get the Fleece value, and:
      1.1. Compute a vector from this value
      1.2. Call `setVectorAt` with the resulting vector, or with nullptr if none.
    2. Call `finish` to apply the updates to the index.
    3. Release the `C4LazyIndexUpdate`, of course. */
CBL_CORE_API C4LazyIndexUpdate* C4NULLABLE c4lazyindex_beginUpdate(C4LazyIndex*, size_t limit, C4Error* outError) C4API;

/** The number of vectors to compute. */
CBL_CORE_API size_t c4lazyindexupdate_count(C4LazyIndexUpdate*) C4API;

/** Returns the i'th value to compute a vector from.
    This is _not_ the entire document, just the value of the expression in the index spec. */
CBL_CORE_API FLValue c4lazyindexupdate_valueAt(C4LazyIndexUpdate*, size_t i) C4API;

/** Sets the vector for the i'th value. If you don't call this, it's assumed there is no
    vector, and any existing vector will be removed upon `finish`. */
CBL_CORE_API bool c4lazyindexupdate_setVectorAt(C4LazyIndexUpdate*, size_t i, const float vector[_Nonnull],
                                                size_t dimension, C4Error* outError) C4API;

/** Updates the index with the computed vectors, removes any index rows for which no vector
    was given, and updates the index's latest sequence.
    @returns  True if the index is now completely up-to-date; false if there have been
              changes to the Collection since the LazyIndexUpdate was created. */
CBL_CORE_API bool c4lazyindexupdate_finish(C4LazyIndexUpdate*, C4Error* outError) C4API;

#endif  // COUCHBASE_ENTERPRISE

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
