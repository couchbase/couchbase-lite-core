//
// c4Query.h
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "c4QueryTypes.h"
#include "fleece/Fleece.h"

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
    C4Query* c4query_new2(C4Database *database,
                          C4QueryLanguage language,
                          C4String expression,
                          int* C4NULLABLE outErrorPos,
                          C4Error* C4NULLABLE error) C4API;

    /** Returns a string describing the implementation of the compiled query.
        This is intended to be read by a developer for purposes of optimizing the query, especially
        to add database indexes. */
    C4StringResult c4query_explain(C4Query*) C4API;


    /** Returns the number of columns (the values specified in the WHAT clause) in each row. */
    unsigned c4query_columnCount(C4Query*) C4API;

    /** Returns a suggested title for a column, which may be:
         * An alias specified in an 'AS' modifier in the column definition
         * A property name
         * A function/operator that computes the column value, e.g. 'MAX()' or '+'
        Each column's title is unique. If multiple columns would have the same title, the
        later ones (in numeric order) will have " #2", "#3", etc. appended. */
    FLString c4query_columnTitle(C4Query*, unsigned column) C4API;


    //////// RUNNING QUERIES:


    /** Sets the parameter values to use when running the query, if no parameters are given to
        \ref c4query_run.
        @param query  The compiled query to run.
        @param encodedParameters  JSON- or Fleece-encoded dictionary whose keys correspond
                to the named parameters in the query expression, and values correspond to the
                values to bind. Any unbound parameters will be `null`. */
    void c4query_setParameters(C4Query *query,
                               C4String encodedParameters) C4API;


    /** Runs a compiled query.
        NOTE: Queries will run much faster if the appropriate properties are indexed.
        Indexes must be created explicitly by calling `c4db_createIndex`.
        @param query  The compiled query to run.
        @param options  Query options; currently unused, just pass NULL.
        @param encodedParameters  Options parameter values; if this parameter is not NULL,
                        it overrides the parameters assigned by \ref c4query_setParameters.
        @param outError  On failure, will be set to the error status.
        @return  An enumerator for reading the rows, or NULL on error. */
    C4QueryEnumerator* c4query_run(C4Query *query,
                                   const C4QueryOptions* C4NULLABLE options,
                                   C4String encodedParameters,
                                   C4Error* C4NULLABLE outError) C4API;

    /** Given a C4FullTextMatch from the enumerator, returns the entire text of the property that
        was matched. (The result depends only on the term's `dataSource` and `property` fields,
        so if you get multiple matches of the same property in the same document, you can skip
        redundant calls with the same values.)
        To find the actual word that was matched, use the term's `start` and `length` fields
        to get a substring of the returned (UTF-8) string. */
    C4StringResult c4query_fullTextMatched(C4Query *query,
                                           const C4FullTextMatch *term,
                                           C4Error* C4NULLABLE outError) C4API;

    /** Advances a query enumerator to the next row, populating its fields.
        Returns true on success, false at the end of enumeration or on error. */
    bool c4queryenum_next(C4QueryEnumerator *e,
                          C4Error* C4NULLABLE outError) C4API;

    /** Returns the total number of rows in the query, if known.
        Not all query enumerators may support this (but the current implementation does.)
        @param e  The query enumerator
        @param outError  On failure, an error will be stored here (probably kC4ErrorUnsupported.)
        @return  The number of rows, or -1 on failure. */
    int64_t c4queryenum_getRowCount(C4QueryEnumerator *e,
                                     C4Error* C4NULLABLE outError) C4API;

    /** Jumps to a specific row. Not all query enumerators may support this (but the current
        implementation does.)
        @param e  The query enumerator
        @param rowIndex  The number of the row, starting at 0, or -1 to restart before first row
        @param outError  On failure, an error will be stored here (probably kC4ErrorUnsupported.)
        @return  True on success, false on failure. */
    bool c4queryenum_seek(C4QueryEnumerator *e,
                          int64_t rowIndex,
                          C4Error* C4NULLABLE outError) C4API;

    /** Restarts the enumeration, as though it had just been created: the next call to
        \ref c4queryenum_next will read the first row, and so on from there. */
    static inline bool c4queryenum_restart(C4QueryEnumerator *e,
                                           C4Error* C4NULLABLE outError) C4API
    { return c4queryenum_seek(e, -1, outError); }

    /** Checks whether the query results have changed since this enumerator was created;
        if so, returns a new enumerator. Otherwise returns NULL. */
    C4QueryEnumerator* c4queryenum_refresh(C4QueryEnumerator *e,
                                           C4Error* C4NULLABLE outError) C4API;

    /** Closes an enumerator without freeing it. This is optional, but can be used to free up
        resources if the enumeration has not reached its end, but will not be freed for a while. */
    void c4queryenum_close(C4QueryEnumerator*) C4API;


    /** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
