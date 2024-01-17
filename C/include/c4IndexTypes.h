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
typedef C4_ENUM(uint32_t, C4VectorMetricType){
        kC4VectorMetricDefault,    ///< Use default metric, Euclidean
        kC4VectorMetricEuclidean,  ///< Euclidean distance (squared)
        kC4VectorMetricCosine,     ///< Cosine distance (1.0 - cosine similarity)
};                                 // Values must match IndexSpec::VectorOptions::MetricType

/** Types of clustering in vector indexes. There is no default type because you must fill in
    the C4VectorClustering struct with a number of centroids or subquantizers+bits. */
typedef C4_ENUM(uint32_t, C4VectorClusteringType){
        kC4VectorClusteringFlat,   ///< Flat k-means clustering
        kC4VectorClusteringMulti,  ///< Inverted Multi-Index clustering
};                                 // Values must match IndexSpec::VectorOptions::ClusteringType

/** Types of encoding (compression) to use in vector indexes. */
typedef C4_ENUM(uint32_t, C4VectorEncodingType){
        kC4VectorEncodingDefault,  ///< Use default encoding, which is currently SQ8
        kC4VectorEncodingNone,     ///< No encoding: 32 bits per dimension, no data loss
        kC4VectorEncodingPQ,       ///< Product Quantizer
        kC4VectorEncodingSQ,       ///< Scalar Quantizer
};                                 // Values must match IndexSpec::VectorOptions::EncodingType

/** Clustering options for vector indexes. */
typedef struct C4VectorClustering {
    C4VectorClusteringType type;                 ///< Clustering type: flat or multi
    unsigned               flat_centroids;       ///< Number of centroids (for flat)
    unsigned               multi_subquantizers;  ///< Number of pieces to split vectors into (for multi)
    unsigned               multi_bits;           ///< log2 of # of centroids per subquantizer (for multi)
} C4VectorClustering;

/** Encoding options for vector indexes. */
typedef struct C4VectorEncoding {
    C4VectorEncodingType type;              ///< Encoding type: default, none, PQ, SQ
    unsigned             pq_subquantizers;  ///< Number of subquantizers (when type is PQ)
    unsigned             bits;              ///< Number of bits (when type is PQ or SQ)
} C4VectorEncoding;

/** Top-level options for vector indexes. */
typedef struct C4VectorIndexOptions {
    C4VectorMetricType metric;           ///< Distance metric
    C4VectorClustering clustering;       ///< Clustering type & parameters
    C4VectorEncoding   encoding;         ///< Vector compression type & parameters
    unsigned           minTrainingSize;  ///< Minimum # of vectors to train index (0 for default)
    unsigned           maxTrainingSize;  ///< Maximum # of vectors to train index on (0 for default)
    unsigned           numProbes;        ///< Number of probes when querying (0 for default)
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
