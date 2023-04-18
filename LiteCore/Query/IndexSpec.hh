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
        enum Type {
            kValue,       ///< Regular index of property value
            kFullText,    ///< Full-text index, for MATCH queries
            kArray,       ///< Index of array values, for UNNEST queries
            kPredictive,  ///< Index of prediction results
        };

        struct Options {
            const char* language;          ///< NULL or an ISO language code ("en", etc)
            bool        ignoreDiacritics;  ///< True to strip diacritical marks/accents from letters
            bool        disableStemming;   ///< Disables stemming
            const char* stopWords;         ///< NULL for default, or comma-delimited string, or empty
        };

        IndexSpec(std::string name_, Type type_, alloc_slice expression_,
                  QueryLanguage queryLanguage = QueryLanguage::kJSON, const Options* opt = nullptr);

        IndexSpec(const IndexSpec&) = delete;
        IndexSpec(IndexSpec&&);

        ~IndexSpec();

        void validateName() const;

        const char* typeName() const {
            static const char* kTypeName[] = {"value", "full-text", "array", "predictive"};
            return kTypeName[type];
        }

        const Options* optionsPtr() const { return options ? &*options : nullptr; }

        /** The required WHAT clause: the list of expressions to index */
        const fleece::impl::Array* NONNULL what() const;

        /** The optional WHERE clause: the condition for a partial index */
        const fleece::impl::Array* where() const;

        std::string const            name;
        Type const                   type;
        alloc_slice const            expression;
        QueryLanguage                queryLanguage;
        std::optional<Options> const options;

      private:
        fleece::impl::Doc* doc() const;

        mutable Retained<fleece::impl::Doc> _doc;
    };

}  // namespace litecore
