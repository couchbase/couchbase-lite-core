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
#include "StringUtil.hh"

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
        if ( (type == kFullText && whichOpts != 1 && whichOpts != 0) || (type == kVector && whichOpts != 2)
             || (type == kArray && whichOpts != 3) )
            error::_throw(error::LiteCoreError::InvalidParameter, "Invalid options type for index");
    }

    IndexSpec::IndexSpec(IndexSpec&& spec)
        : IndexSpec(std::move(spec.name), spec.type, std::move(spec.expression), spec.queryLanguage,
                    std::move(spec.options)) {
        _doc      = spec._doc;
        spec._doc = nullptr;
    }

    IndexSpec::~IndexSpec() {
        FLDoc_Release(_doc);
        FLDoc_Release(_unnestDoc);
    }

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
                        if ( auto doc = Doc::fromJSON(expression); doc ) _doc = doc.detach();
                        else
                            error::_throw(error::InvalidQuery, "Invalid JSON in index expression");
                        break;
                    }
                case QueryLanguage::kN1QL:
                    try {
                        alloc_slice json;
                        if ( !expression.empty() ) {
                            int           errPos;
                            FLMutableDict result = n1ql::parse(string(expression), &errPos);
                            if ( !result ) { throw Query::parseError("N1QL syntax error in index expression", errPos); }
                            json = FLValue_ToJSON(FLValue(result));
                            FLMutableDict_Release(result);
                        } else {
                            // n1ql parser won't compile empty string to empty array. Do it manually.
                            json = "[]";
                        }
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
        // Array Inddex can have empty what.
        if ( type != kArray && what.empty() ) error::_throw(error::InvalidQuery, "Index WHAT list cannot be empty");
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

    // Turning unnestPath in C4IndexOptions to an array in JSON expresion.
    // Ex. students[].interests -> [[".students"],[".interests"]]
    FLArray IndexSpec::unnestPaths() const {
        const ArrayOptions* arrayOpts = arrayOptions();
        if ( !arrayOpts || !arrayOpts->unnestPath )
            error::_throw(error::InvalidParameter, "IndexOptions for ArrayIndex must include unnestPath.");

        Doc doc(unnestDoc());
        if ( auto dict = doc.asDict(); dict ) {
            if ( auto whatVal = qt::getCaseInsensitive(dict, "WHAT"); whatVal )
                return qt::requiredArray(whatVal, "Index WHAT term");
        }
        return nullptr;
    }

    FLDoc IndexSpec::unnestDoc() const {
        // Precondition: arrayOptions() && arrayOptions()->unnestPath
        if ( !_unnestDoc ) {
            try {
                string n1qlUnnestPaths{arrayOptions()->unnestPath};
                if ( n1qlUnnestPaths.empty() )
                    error::_throw(error::InvalidParameter,
                                  "IndexOptions for ArrayIndex must have non-empty unnestPath.");

                // Turning "students[].interests" to "students, interests"
                litecore::replace(n1qlUnnestPaths, KeyStore::kUnnestLevelSeparator, ", ");
                int           errPos;
                FLMutableDict result = n1ql::parse(n1qlUnnestPaths, &errPos);
                if ( !result ) {
                    string msg = "N1QL syntax error in unnestPath \"" + n1qlUnnestPaths + "\"";
                    throw Query::parseError(msg.c_str(), errPos);
                }

                alloc_slice json{FLValue_ToJSON(FLValue(result))};
                FLMutableDict_Release(result);
                _unnestDoc = Doc::fromJSON(json).detach();
            } catch ( const std::runtime_error& exc ) {
                error::_throw(error::InvalidQuery, "Invalid N1QL in unnestPath (%s)", exc.what());
            }
        }
        return _unnestDoc;
    }
}  // namespace litecore
