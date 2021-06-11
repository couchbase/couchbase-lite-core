//
// IndexSpec.hh
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
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
#include "Base.hh"
#include <optional>
#include <string>

namespace fleece::impl {
    class Array;
    class Doc;
}

namespace litecore {
    
    enum class QueryLanguage {          // Values MUST match C4QueryLanguage in c4Query.h
        kJSON,
        kN1QL,
    };

    struct IndexSpec {
        enum Type {
            kValue,         ///< Regular index of property value
            kFullText,      ///< Full-text index, for MATCH queries
            kArray,         ///< Index of array values, for UNNEST queries
            kPredictive,    ///< Index of prediction results
        };

        struct Options {
            const char* language;   ///< NULL or an ISO language code ("en", etc)
            bool ignoreDiacritics;  ///< True to strip diacritical marks/accents from letters
            bool disableStemming;   ///< Disables stemming
            const char* stopWords;  ///< NULL for default, or comma-delimited string, or empty
        };

        IndexSpec(std::string name_,
                  Type type_,
                  alloc_slice expression_,
                  QueryLanguage queryLanguage =QueryLanguage::kJSON,
                  const Options* opt =nullptr);

        IndexSpec(const IndexSpec&) =delete;
        IndexSpec(IndexSpec&&);

        ~IndexSpec();

        void validateName() const;

        const char* typeName() const {
            static const char* kTypeName[] = {"value", "full-text", "array", "predictive"};
            return kTypeName[type];
        }

        const Options* optionsPtr() const       {return options ? &*options : nullptr;}

        /** The required WHAT clause: the list of expressions to index */
        const fleece::impl::Array* NONNULL what() const;

        /** The optional WHERE clause: the condition for a partial index */
        const fleece::impl::Array* where() const;

        std::string const            name;
        Type        const            type;
        alloc_slice const            expression;
        QueryLanguage                queryLanguage;
        std::optional<Options> const options;

    private:
        fleece::impl::Doc* doc() const;

        mutable Retained<fleece::impl::Doc> _doc;
    };

}
