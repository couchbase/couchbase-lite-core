//
// c4IndexTypes.h
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
#include "c4QueryTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup Indexing  Database Indexes
 @{ */


/** Types of indexes. */
typedef C4_ENUM(uint32_t, C4IndexType){
        kC4ValueIndex,       ///< Regular index of property value
        kC4FullTextIndex,    ///< Full-text index
        kC4ArrayIndex,       ///< Index of array values, for use with UNNEST
        kC4PredictiveIndex,  ///< Index of prediction() results (Enterprise Edition only)
        kC4VectorIndex,      ///< Index of ML vector similarity (Enterprise Edition only)
};

/** Distance metric to use in vector indexes. */
typedef C4_ENUM(uint32_t, C4VectorMetric){
        kC4VectorMetricDefault,    ///< Use default metric, Euclidean
        kC4VectorMetricEuclidean,  ///< Euclidean distance (squared)
        kC4VectorMetricCosine,     ///< Cosine distance (1.0 - cosine similarity)
};                                 // Values must match IndexSpec::VectorOptions::Metric

/** Types of encoding (compression) to use in vector indexes. */
typedef C4_ENUM(uint32_t, C4VectorEncoding){
        kC4VectorEncodingDefault,  ///< Use default encoding, which is currently SQ8
        kC4VectorEncodingNone,     ///< No encoding: 32 bits per dimension, no data loss
        kC4VectorEncodingSQ8,      ///< Scalar Quantizer: 8 bits per dimension (default)
        kC4VectorEncodingSQ6,      ///< Scalar Quantizer: 6 bits per dimension
        kC4VectorEncodingSQ4,      ///< Scalar Quantizer: 4 bits per dimension
};                                 // Values must match IndexSpec::VectorOptions::Encoding

/** Options for vector indexes. */
typedef struct C4VectorIndexOptions {
    unsigned         numCentroids;     ///< Number of buckets to partition the vectors between
    C4VectorMetric   metric;           ///< Distance metric
    C4VectorEncoding encoding;         ///< Vector compression type
    unsigned         minTrainingSize;  ///< Minimum # of vectors to train index (>= 25*numCentroids)
    unsigned         maxTrainingSize;  ///< Maximum # of vectors to train index on (<= 256*numCentroids)
} C4VectorIndexOptions;

/** Options for indexes; these each apply to specific types of indexes. */
typedef struct C4IndexOptions {
    /** Dominant language of text to be indexed; setting this enables word stemming, i.e.
        matching different cases of the same word ("big" and "bigger", for instance.)
        Can be an ISO-639 language code or a lowercase (English) language name; supported
        languages are: da/danish, nl/dutch, en/english, fi/finnish, fr/french, de/german,
        hu/hungarian, it/italian, no/norwegian, pt/portuguese, ro/romanian, ru/russian,
        es/spanish, sv/swedish, tr/turkish.
        If left null,  or set to an unrecognized language, no language-specific behaviors
        such as stemming and stop-word removal occur. */
    const char* C4NULLABLE language;

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
    const char* C4NULLABLE stopWords;

    /** Options for vector indexes. */
    C4VectorIndexOptions vector;
} C4IndexOptions;

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
