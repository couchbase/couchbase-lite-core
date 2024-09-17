//
// IndexSpec.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include <utility>
#include "IndexSpec.hh"
#include "TranslatorUtils.hh"
#include "Error.hh"
#include "Doc.hh"
#include "fleece/Fleece.hh"
#include "n1ql_parser.hh"
#include "Query.hh"
#include "MutableDict.hh"

namespace litecore {
    using namespace fleece;

    IndexSpec::IndexSpec(std::string name_, Type type_, alloc_slice expression_, QueryLanguage queryLanguage_,
                         Options opt)
        : name(std::move(name_))
        , type(type_)
        , expression(std::move(expression_))
        , queryLanguage(queryLanguage_)
        , options(std::move(opt)) {
        auto whichOpts = options.index();
        if ( (type == kFullText && whichOpts != 1 && whichOpts != 0) || (type == kVector && whichOpts != 2) )
            error::_throw(error::LiteCoreError::InvalidParameter, "Invalid options type for index");
    }

    IndexSpec::IndexSpec(IndexSpec&& spec)
        : IndexSpec(std::move(spec.name), spec.type, std::move(spec.expression), spec.queryLanguage,
                    std::move(spec.options)) {
        _doc      = spec._doc;
        spec._doc = nullptr;
    }

    IndexSpec::~IndexSpec() { FLDoc_Release(_doc); }

    void IndexSpec::validateName() const {
        if ( name.empty() ) { error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not be empty"); }
        if ( slice(name).findByte((uint8_t)'"') != nullptr ) {
            error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not contain "
                                                                  "the double quote (\") character");
        }
    }

    FLDoc IndexSpec::doc() const {
        if ( !_doc ) {
            switch ( queryLanguage ) {
                case QueryLanguage::kJSON:
                    {
                        _doc = Doc::fromJSON(expression).detach();
                        if ( !_doc ) error::_throw(error::InvalidQuery, "Invalid JSON in index expression");
                        break;
                    }
                case QueryLanguage::kN1QL:
                    try {
                        int           errPos;
                        FLMutableDict result = n1ql::parse(string(expression), &errPos);
                        if ( !result ) { throw Query::parseError("N1QL syntax error in index expression", errPos); }
                        alloc_slice json(FLValue_ToJSON(FLValue(result)));
                        FLMutableDict_Release(result);
                        _doc = Doc::fromJSON(json).detach();
                    } catch ( const std::runtime_error& ) {
                        error::_throw(error::InvalidQuery, "Invalid N1QL in index expression");
                    }
                    break;
            }
        }
        return _doc;
    }

    FLArray IndexSpec::what() const {
        Doc   doc(this->doc());
        Array what;
        if ( auto dict = doc.asDict() ) {
            what = qt::requiredArray(qt::getCaseInsensitive(dict, "WHAT"), "Index WHAT term");
        } else {
            // For backward compatibility, the JSON is allowed to be just an array
            // of expressions.
            what = qt::requiredArray(doc.root(), "Index JSON");
        }
        if ( what.empty() ) error::_throw(error::InvalidQuery, "Index WHAT list cannot be empty");
        return what;
    }

    FLArray IndexSpec::where() const {
        Doc doc(this->doc());
        if ( auto dict = doc.asDict() ) {
            if ( auto whereVal = qt::getCaseInsensitive(dict, "WHERE"); whereVal )
                return qt::requiredArray(whereVal, "Index WHERE term");
        }
        return nullptr;
    }


}  // namespace litecore
