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
#include "VectorIndexSpec.hh"
#include "fleece/Fleece.h"
#include <optional>
#include <string>
#include <variant>

namespace litecore {

    enum class QueryLanguage {  // Values MUST match C4QueryLanguage in c4Query.h
        kJSON,
        kN1QL,
    };

    struct IndexSpec {
        /// The types of indexes. (Values MUST match C4IndexType)
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

        /// Options for an ArrayIndex
        struct ArrayOptions {
            alloc_slice unnestPath;

            ArrayOptions(string_view unnestPath_) : unnestPath(alloc_slice::nullPaddedString(unnestPath_)) {}
        };

        /// Options for a vector index.
        using VectorOptions = vectorsearch::IndexSpec;

        static constexpr vectorsearch::SQEncoding DefaultEncoding{8};

        /// Index options. If not empty (the first state), must match the index type.
        using Options = std::variant<std::monostate, FTSOptions, VectorOptions, ArrayOptions>;

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

        const ArrayOptions* arrayOptions() const { return std::get_if<ArrayOptions>(&options); }

        /** The required WHAT clause: the list of expressions to index */
        FLArray what() const;

        /** The optional WHERE clause: the condition for a partial index */
        FLArray where() const;

        /** The nested unnestPath from arrayOptions, as separated by "[]." is turned to an array. */
        FLArray unnestPaths() const;

        std::string const name;           ///< Name of index
        Type const        type;           ///< Type of index
        alloc_slice const expression;     ///< The query expression
        QueryLanguage     queryLanguage;  ///< Is expression JSON or N1QL?
        Options const     options;        ///< Options for FTS and vector indexes

      private:
        FLDoc doc() const;
        FLDoc unnestDoc() const;

        mutable FLDoc _doc = nullptr;
        mutable FLDoc _unnestDoc = nullptr;
    };

}  // namespace litecore
