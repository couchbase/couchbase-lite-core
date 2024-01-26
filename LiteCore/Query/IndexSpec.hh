//
// IndexSpec.hh
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
#include "Base.hh"
#include <optional>
#include <string>
#include <variant>

namespace fleece::impl {
    class Array;
    class Doc;
}  // namespace fleece::impl

namespace litecore {

    enum class QueryLanguage {  // Values MUST match C4QueryLanguage in c4Query.h
        kJSON,
        kN1QL,
    };

    struct IndexSpec {
        /// The types of indexes.
        enum Type {
            kValue,       ///< Regular index of property value
            kFullText,    ///< Full-text index, for MATCH queries. Uses IndexSpec::FTSOptions.
            kArray,       ///< Index of array values, for UNNEST queries
            kPredictive,  ///< Index of prediction results
            kVector,      ///< Index of ML vector similarity. Uses IndexSpec::VectorOptions.
        };

        /// Options for a full-text index.
        struct FTSOptions {
            const char* language{};          ///< NULL or an ISO language code ("en", etc)
            bool        ignoreDiacritics{};  ///< True to strip diacritical marks/accents from letters
            bool        disableStemming{};   ///< Disables stemming
            const char* stopWords{};         ///< NULL for default, or comma-delimited string, or empty
        };

        /// Options for a vector index.
        struct VectorOptions {
            enum MetricType {
                DefaultMetric,  ///< Use default metric, Euclidean
                Euclidean,      ///< Euclidean distance (squared)
                Cosine,         ///< Cosine distance (1.0 - cosine similarity)
            };                  // Note: values must match C4VectorMetricType in c4IndexTypes.h

            enum ClusteringType {
                Flat,
                Multi,
            };  // Note: values must match C4VectorClusteringType in c4IndexTypes.h

            enum EncodingType {
                DefaultEncoding,  ///< Use default encoding, which is currently SQ8Bit
                NoEncoding,       ///< No encoding; 4 bytes per dimension, no data loss
                PQ,               ///< Product Quantizer
                SQ,               ///< Scalar Quantizer
            };                    // Note: values must match C4VectorEncodingType in c4IndexTypes.h

            struct Clustering {
                ClusteringType type;
                unsigned       flat_centroids;
                unsigned       multi_subquantizers;  ///< Number of pieces to split vectors into (for multi)
                unsigned       multi_bits;           ///< log2 of # of centroids per subquantizer (for multi)
            };

            struct Encoding {
                EncodingType type;              ///< Encoding type: none, PQ, SQ
                unsigned     pq_subquantizers;  ///< Number of subquantizers (for PQ)
                unsigned     bits;              ///< Number of bits (for PQ and SQ)
            };

            unsigned   dimensions;                 ///< Number of dimensions
            MetricType metric{DefaultMetric};      ///< Distance metric
            Clustering clustering{Flat};           ///< Clustering type & parameters
            Encoding   encoding{DefaultEncoding};  ///< Vector compression type & parameters
            ///<
            unsigned minTrainingSize{0};  ///< Minimum # of vectors to train index (>= 25*numCentroids)
            unsigned maxTrainingSize{0};  ///< Maximum # of vectors to train index on (<= 256*numCentroids)
            unsigned numProbes{0};        ///< Default # of probes when querying

            /// Constructor. Number of dimensions is a required parameter.
            explicit VectorOptions(unsigned d) : dimensions(d) {}
        };

        /// Index options. If not empty (the first state), must match the index type.
        using Options = std::variant<std::monostate, FTSOptions, VectorOptions>;

        /// Constructs an index spec.
        /// @param name_  Name of the index (must be unique in its collection.)
        /// @param type_  Type of the index.
        /// @param expression_  The value(s) to be indexed.
        /// @param queryLanguage  Language used for `expression_`; either JSON or N1QL.
        /// @param options_  Options; if given, its type must match the index type.
        IndexSpec(std::string name_, Type type_, alloc_slice expression_,
                  QueryLanguage queryLanguage = QueryLanguage::kJSON, Options options_ = {});

        IndexSpec(const IndexSpec&) = delete;
        IndexSpec(IndexSpec&&);

        ~IndexSpec();

        void validateName() const;

        const char* typeName() const {
            static const char* kTypeName[] = {"value", "full-text", "array", "predictive", "vector"};
            return kTypeName[type];
        }

        const FTSOptions* ftsOptions() const { return std::get_if<FTSOptions>(&options); }

        const VectorOptions* vectorOptions() const { return std::get_if<VectorOptions>(&options); }

        /** The required WHAT clause: the list of expressions to index */
        const fleece::impl::Array* NONNULL what() const;

        /** The optional WHERE clause: the condition for a partial index */
        const fleece::impl::Array* where() const;

        std::string const name;           ///< Name of index
        Type const        type;           ///< Type of index
        alloc_slice const expression;     ///< The query expression
        QueryLanguage     queryLanguage;  ///< Is expression JSON or N1QL?
        Options const     options;        ///< Options for FTS and vector indexes

      private:
        fleece::impl::Doc* doc() const;

        mutable Retained<fleece::impl::Doc> _doc;
    };

}  // namespace litecore
