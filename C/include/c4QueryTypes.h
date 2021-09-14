//
// c4QueryTypes.h
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
#include "c4Base.h"
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup QueryingDB Querying the Database
    @{ */


/** Supported query languages. */
typedef C4_ENUM(uint32_t, C4QueryLanguage) {
    kC4JSONQuery,   ///< JSON query schema as documented in LiteCore wiki
    kC4N1QLQuery,   ///< N1QL syntax (a large subset)
};


/** Options for running queries. */
typedef struct {
    bool rankFullText_DEPRECATED;      ///< Ignored; use the `rank()` query function instead.
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
    Created by c4db_query. Must be freed with c4queryenum_release.
    The fields of this struct represent the current matched index row, and are valid until the
    next call to c4queryenum_next or c4queryenum_release. */
struct C4QueryEnumerator {
    /** The columns of this result, in the same order as in the query's `WHAT` clause. */
    FLArrayIterator columns;

    /** A bitmap where a 1 bit represents a column whose value is MISSING.
        This is how you tell a missing property value from a value that's JSON 'null',
        since the value in the `columns` array will be a Fleece `null` either way. */
    uint64_t missingColumns;

    /** The number of full-text matches (i.e. the number of items in `fullTextMatches`) */
    uint32_t fullTextMatchCount;

    /** Array with details of each full-text match */
    const C4FullTextMatch* C4NULLABLE fullTextMatches;
};


/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
