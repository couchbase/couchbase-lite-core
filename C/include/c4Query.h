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
#include "c4Database.h"
#include "c4Document.h"
#include "Fleece.h"

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
        @param expression  JSON data describing the query. (Schema is documented elsewhere.)
        @param error  Error will be written here if the function fails.
        @result  A new C4Query, or NULL on failure. */
    C4Query* c4query_new(C4Database *database,
                         C4String expression,
                         C4Error *error) C4API;

    /** Frees a query.  It is legal to pass NULL. */
    void c4query_free(C4Query*) C4API;

    /** Returns a string describing the implementation of the compiled query.
        This is intended to be read by a developer for purposes of optimizing the query, especially
        to add database indexes. */
    C4StringResult c4query_explain(C4Query *query) C4API;


    /** Returns the number of columns (the values specified in the WHAT clause) in each row. */
    unsigned c4query_columnCount(C4Query *query) C4API;


    //////// RUNNING QUERIES:


    /** Options for running queries. */
    typedef struct {
        bool rankFullText;      ///< Should full-text results be ranked by relevance?
    } C4QueryOptions;


    /** Default query options. Has skip=0, limit=UINT_MAX, rankFullText=true. */
	CBL_CORE_API extern const C4QueryOptions kC4DefaultQueryOptions;


    /** Info about a match of a full-text query term. */
    typedef struct {
        uint64_t dataSource;    ///< Opaque identifier of where text is stored
        uint32_t property;      ///< Which property in the index was matched (array index in `expressionsJSON`)
        uint32_t term;          ///< Which search term (word) in the query was matched
        uint32_t start;         ///< *Byte* range start of the match in the full text
        uint32_t length;        ///< *Byte* range length of the match in the full text
    } C4FullTextMatch;


    /** A query result enumerator.
        Created by c4db_query. Must be freed with c4queryenum_free.
        The fields of this struct represent the current matched index row, and are valid until the
        next call to c4queryenum_next or c4queryenum_free. */
    typedef struct {
        /** The columns of this result, in the same order as in the query's `WHAT` clause. */
        FLArrayIterator columns;

        /** A bitmap where a 1 bit represents a column whose value is MISSING.
            This is how you tell a missing property value from a value that's JSON 'null',
            since the value in the `columns` array will be a Fleece `null` either way. */
        uint64_t missingColumns;

        /** The number of full-text matches (i.e. the number of items in `fullTextMatches`) */
        uint32_t fullTextMatchCount;

        /** Array with details of each full-text match */
        const C4FullTextMatch *fullTextMatches;
    } C4QueryEnumerator;


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
    C4QueryEnumerator* c4query_run(C4Query *query C4NONNULL,
                                   const C4QueryOptions *options,
                                   C4String encodedParameters,
                                   C4Error *outError) C4API;

    /** Given a C4FullTextMatch from the enumerator, returns the entire text of the property that
        was matched. (The result depends only on the term's `dataSource` and `property` fields,
        so if you get multiple matches of the same property in the same document, you can skip
        redundant calls with the same values.)
        To find the actual word that was matched, use the term's `start` and `length` fields
        to get a substring of the returned (UTF-8) string. */
    C4StringResult c4query_fullTextMatched(C4Query *query C4NONNULL,
                                           const C4FullTextMatch *term C4NONNULL,
                                           C4Error *outError) C4API;

    /** Advances a query enumerator to the next row, populating its fields.
        Returns true on success, false at the end of enumeration or on error. */
    bool c4queryenum_next(C4QueryEnumerator *e C4NONNULL,
                          C4Error *outError) C4API;

    /** Returns the total number of rows in the query, if known.
        Not all query enumerators may support this (but the current implementation does.)
        @param e  The query enumerator
        @param outError  On failure, an error will be stored here (probably kC4ErrorUnsupported.)
        @return  The number of rows, or -1 on failure. */
    int64_t c4queryenum_getRowCount(C4QueryEnumerator *e C4NONNULL,
                                     C4Error *outError) C4API;

    /** Jumps to a specific row. Not all query enumerators may support this (but the current
        implementation does.)
        @param e  The query enumerator
        @param rowIndex  The number of the row, starting at 0
        @param outError  On failure, an error will be stored here (probably kC4ErrorUnsupported.)
        @return  True on success, false on failure. */
    bool c4queryenum_seek(C4QueryEnumerator *e C4NONNULL,
                          uint64_t rowIndex,
                          C4Error *outError) C4API;

    /** Checks whether the query results have changed since this enumerator was created;
        if so, returns a new enumerator. Otherwise returns NULL. */
    C4QueryEnumerator* c4queryenum_refresh(C4QueryEnumerator *e C4NONNULL,
                                           C4Error *outError) C4API;

    /** Closes an enumerator without freeing it. This is optional, but can be used to free up
        resources if the enumeration has not reached its end, but will not be freed for a while. */
    void c4queryenum_close(C4QueryEnumerator *e) C4API;

    /** Frees a query enumerator. */
    void c4queryenum_free(C4QueryEnumerator *e) C4API;

    /** @} */


    //////// INDEXES:


    /** \defgroup Indexing  Database Indexes
     @{ */


    /** Types of indexes. */
    typedef C4_ENUM(uint32_t, C4IndexType) {
        kC4ValueIndex,         ///< Regular index of property value
        kC4FullTextIndex,      ///< Full-text index
        kC4GeoIndex,           ///< Geospatial index of GeoJSON values (NOT YET IMPLEMENTED)
    };


    /** Options for indexes; these each apply to specific types of indexes. */
    typedef struct {
        /** Dominant language of text to be indexed; setting this enables word stemming, i.e.
            matching different cases of the same word ("big" and "bigger", for instance.)
            Can be an ISO-639 language code or a lowercase (English) language name; supported
            languages are: da/danish, nl/dutch, en/english, fi/finnish, fr/french, de/german,
            hu/hungarian, it/italian, no/norwegian, pt/portuguese, ro/romanian, ru/russian,
            es/spanish, sv/swedish, tr/turkish.
            If left null,  or set to an unrecognized language, no language-specific behaviors
            such as stemming and stop-word removal occur. */
        const char *language;

        /** Should diacritical marks (accents) be ignored? Defaults to false.
            Generally this should be left false for non-English text. */
        bool ignoreDiacritics;

        /** "Stemming" coalesces different grammatical forms of the same word ("big" and "bigger",
            for instance.) Full-text search normally uses stemming if the language is one for
            which stemming rules are available, but this flag can be set to `true` to disable it.
            Stemming is currently available for these languages: da/danish, nl/dutch, en/english,
            fi/finnish, fr/french, de/german, hu/hungarian, it/italian, no/norwegian, pt/portuguese,
            ro/romanian, ru/russian, s/spanish, sv/swedish, tr/turkish. */
        bool disableStemming;

        /** List of words to ignore ("stop words") for full-text search. Ignoring common words
            like "the" and "a" helps keep down the size of the index.
            If NULL, a default word list will be used based on the `language` option, if there is
            one for that language.
            To suppress stop-words, use an empty string.
            To provide a custom list of words, use a string containing the words in lowercase
            separated by spaces. */
        const char *stopWords;
    } C4IndexOptions;


    /** Creates a database index, of the values of specific expressions across all documents.
        The name is used to identify the index for later updating or deletion; if an index with the
        same name already exists, it will be replaced unless it has the exact same expressions.

        Currently two types of indexes are supported:

        * Value indexes speed up queries by making it possible to look up property (or expression)
          values without scanning every document. They're just like regular indexes in SQL or N1QL.
          Multiple expressions are supported; the first is the primary key, second is secondary.
          Expressions must evaluate to scalar types (boolean, number, string).
        * Full-Text Search (FTS) indexes enable fast search of natural-language words or phrases
          by using the `MATCH` operator in a query. A FTS index is **required** for full-text
          search: a query with a `MATCH` operator will fail to compile unless there is already a
          FTS index for the property/expression being matched. Only a single expression is
          currently allowed, and it must evaluate to a string.

        Note: If the value of an expression in some document is missing or an unsupported type,
        that document will just be omitted from the index. It's not an error.

        Expressions are defined in JSON, as in a query, and wrapped in a JSON array. For example,
        `[[".name.first"]]` will index on the first-name property. Note the two levels of brackets,
        since an expression is already an array.

        Currently, full-text indexes are limited to a single expression only.
        Geospatial indexes are not implemented at all yet.

        @param database  The database to index.
        @param name  The name of the index. Any existing index with the same name will be replaced,
                     unless it has the identical expressions (in which case this is a no-op.)
        @param expressionsJSON  A JSON array of one or more expressions to index. Each expression
                     takes the same form as in a query, which means it's a JSON array as well;
                     don't get mixed up by the nesting!
        @param indexType  The type of index (value or full-text.)
        @param indexOptions  Options for the index. If NULL, each option will get a default value.
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_createIndex(C4Database *database C4NONNULL,
                          C4String name,
                          C4String expressionsJSON,
                          C4IndexType indexType,
                          const C4IndexOptions *indexOptions,
                          C4Error *outError) C4API;

    /** Deletes an index that was created by `c4db_createIndex`.
        @param database  The database to index.
        @param name The name of the index to delete
        @param outError  On failure, will be set to the error status.
        @return  True on success, false on failure. */
    bool c4db_deleteIndex(C4Database *database C4NONNULL,
                          C4String name,
                          C4Error *outError) C4API;
    
    /** Returns the names of all indexes in the database.
        @param database  The database to check
        @param outError  On failure, will be set to the error status.
        @return  A Fleece-encoded array of strings, or NULL on failure. */
    C4SliceResult c4db_getIndexes(C4Database* database C4NONNULL,
                                  C4Error* outError) C4API;

    /** @} */

#ifdef __cplusplus
}
#endif
