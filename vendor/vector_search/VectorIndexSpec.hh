//
// VectorIndexSpec.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// NOTE: This file appears in both the vectorsearch and couchbase-lite-core repos.
// Any changes made in one should be copied to the other!

#pragma once
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace vectorsearch {

    /// Distance metric; defines the distance between vectors.
    enum class Metric {
        Euclidean2,                 ///< Euclidean distance, squared
        Cosine,                     ///< Cosine similarity subtracted from 1, so smaller is closer
        Default = Euclidean2
    };

    struct FlatClustering {
        unsigned numCentroids;      ///< Number of buckets to assign the vectors to
    };

    struct MultiIndexClustering {
        unsigned subquantizers = 2; ///< Number of pieces each vector is split into
        unsigned bitsPerSub    = 8; ///< Number of bits of centroid count per piece
    };

    enum class ClusteringType {Flat, MultiIndex};   ///< Just identifies type of clustering

    using Clustering = std::variant<FlatClustering, MultiIndexClustering>;


    struct NoEncoding { };

    struct PQEncoding {
        unsigned subquantizers;     ///< Number of pieces each vector is split into
        unsigned bitsPerSub = 8;    ///< Bits for encoding each piece

        explicit constexpr PQEncoding(unsigned sub, unsigned bits =8)
        :subquantizers(sub), bitsPerSub(bits) { }
    };

    struct SQEncoding {
        unsigned bitsPerDimension = 8;          ///< Bits/dimension; must be 4, 6 or 8
    };

    enum class EncodingType {None, PQ, SQ};     ///< Just identifies type of encoding

    using Encoding = std::variant<NoEncoding, PQEncoding, SQEncoding>;


    /** The parameters of a VectorDB. */
    struct IndexSpec {

        //---- PROPERTIES:

        unsigned                dimensions = 0;                     ///< Vector dimensions
        Metric                  metric = Metric::Default;           ///< Distance metric
        Clustering              clustering = MultiIndexClustering{};///< Clustering type
        Encoding                encoding = SQEncoding{};            ///< Encoding type

        std::optional<int64_t>  minTrainingCount;       ///< Min vectors needed to train
        std::optional<int64_t>  maxTrainingCount;       ///< Max vectors to train with
        std::optional<unsigned> probeCount;             ///< Number of buckets to probe

        /// If true, inserted vectors are not encoded or mapped to centroids until the next query.
        /// @warning  This is not the same meaning of "lazy" as in CBL! See \ref lazyEmbedding.
        bool                    lazyEncoding = false;

        /// If true, app will use the CBL IndexUpdater API to compute/request vectors for docs.
        /// @note  This flag is ignored by vectorsearch! It's for the use of LiteCore.
        bool                    lazyEmbedding = false;

        /// Set `minTrainingCount` to this value (or greater) to disable automatic training.
        static constexpr int64_t kNeverTrain = 999'999'999;

        //---- CONSTRUCTION:

        IndexSpec() = default;

        IndexSpec(unsigned dim, Clustering q, Encoding e = NoEncoding{})
            :dimensions(dim), clustering(q), encoding(e) { }

        /// Sets an attribute of an IndexSpec from a key/value pair; useful for CLI.
        /// See Extension.md for documentation of the supported keys and values.
        /// @returns true if it applied the param, false if it didn't recognize the key.
        /// @throws std::invalid_argument if the value is invalid.
        [[nodiscard]] bool readArg(std::string_view key, std::string_view value);

        /// Same as the other `readArg` but takes a single string of the form `key=value` or `key`.
        [[nodiscard]] bool readArg(std::string_view arg);

        //---- VALIDATION:

        /// Throws a std::invalid_argument exception if the parameters are invalid.
        /// Also sets reasonable values for training & probe counts, if omitted.
        void validate() const;

        /// Ensures `minTrainingCount` and `maxTrainingCount` are set to reasonable values:
        /// - If either is `nullopt` or 0, it's set to its default (based on the # of centroids.)
        /// - If min is too small, it's raised to the default, and a warning is logged.
        void resolveTrainingCounts();

        //---- ACCESSORS:

        ClusteringType clusteringType() const {return ClusteringType(clustering.index());}
        EncodingType encodingType() const     {return EncodingType(encoding.index());}

        /// The number of centroid points that need to be identified during training.
        /// This depends on both the clustering type and the encoding, because both PQ and SQ
        /// encoders have their own internal sets of centroids.
        /// @warning  FAISS is likely to throw an exception if training is performed with fewer
        ///           vectors than this number.
        unsigned numCentroidsToTrain() const {
            unsigned nCent;
            if (auto q = std::get_if<MultiIndexClustering>(&clustering))
                nCent = 1 << q->bitsPerSub;
            else
                nCent = std::get<FlatClustering>(clustering).numCentroids;
            if (auto pq = std::get_if<PQEncoding>(&encoding)) {
                // PQ encoding has its own centroids that need to be trained:
                nCent = std::max(nCent, 1u << pq->bitsPerSub);
            }
            return nCent;
        }

        /// The number of buckets to which vectors will be assigned when indexed.
        /// @note This is not the same as `numCentroidsToTrain`, because
        ///     (a) with multi-index clustering the 'centroids' used as buckets are actually tuples,
        ///         with one centroid per subquantizer;
        ///     (b) it only refers to the main IVF index, not centroids used by encoders.
        unsigned numCentroids() const {
            if (auto q = std::get_if<MultiIndexClustering>(&clustering))
                return 1 << (q->bitsPerSub * q->subquantizers);
            else
                return std::get<FlatClustering>(clustering).numCentroids;
        }

        //---- ENCODING:

        /// Writes a series of comma-separated "key=value" pairs describing this spec.
        std::ostream& writeArgs(std::ostream&) const;

        /// Returns a string of comma-separated key=value pairs describing this spec.
        std::string createArgs() const;

        friend std::ostream& operator<<(std::ostream& out, IndexSpec const& spec) {
            return spec.writeArgs(out);
        }

        /// Returns a human-readable string describing this spec.
        std::string description() const;

        //---- LIMITS:

        static constexpr unsigned               kMinDimensions = 2;
        static constexpr unsigned               kMaxDimensions = 4096;
        static constexpr FlatClustering         kMinFlatClustering {1};
        static constexpr FlatClustering         kMaxFlatClustering {64'000};
        static constexpr MultiIndexClustering   kMinMultiIndexClustering {   2,  4};
        static constexpr MultiIndexClustering   kMaxMultiIndexClustering {1024, 12};
        static constexpr PQEncoding             kMinPQEncoding {   2,  4};
        static constexpr PQEncoding             kMaxPQEncoding {1024, 12};
        static constexpr SQEncoding             kMinSQEncoding {4};
        static constexpr SQEncoding             kMaxSQEncoding {8};

        /// Absolute minimum number of training vectors needed per centroid.
        /// The `train` method will return false instead of training if given fewer.
        static constexpr int64_t kMinTrainingVectorsPerCentroid = 25;

        /// Minimum recommended (by FAISS) number of training vectors per centroid for good results.
        static constexpr int64_t kRecommendedMinTrainingVectorsPerCentroid = 39;
        static constexpr int64_t kRecommendedMaxTrainingVectorsPerCentroid = 100;

    };

}
