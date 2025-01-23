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
#include "QueryParser+Private.hh"
#include "Error.hh"
#include "Doc.hh"
#include "fleece/FLMutable.h"
#include "n1ql_parser.hh"
#include "Query.hh"
#include "MutableDict.hh"

namespace litecore {
    using namespace fleece;
    using namespace fleece::impl;

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

    IndexSpec::IndexSpec(IndexSpec&&) = default;
    IndexSpec::~IndexSpec()           = default;

    void IndexSpec::validateName() const {
        if ( name.empty() ) { error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not be empty"); }
        if ( slice(name).findByte((uint8_t)'"') != nullptr ) {
            error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not contain "
                                                                  "the double quote (\") character");
        }
    }

    Doc* IndexSpec::doc() const {
        if ( !_doc ) {
            switch ( queryLanguage ) {
                case QueryLanguage::kJSON:
                    try {
                        if ( canPartialIndex() && !whereClause.empty() ) {
                            std::stringstream ss;
                            ss << R"({"WHAT": )" << expression.asString() << R"(, "WHERE": )" << whereClause.asString()
                               << "}";
                            _doc = Doc::fromJSON(ss.str());
                        } else {
                            _doc = Doc::fromJSON(expression);
                        }
                    } catch ( const FleeceException& ) {
                        error::_throw(error::InvalidQuery, "Invalid JSON in index expression");
                    }
                    break;
                case QueryLanguage::kN1QL:
                    try {
                        int         errPos;
                        alloc_slice json;
                        if ( !expression.empty() ) {
                            MutableDict*      result   = nullptr;
                            bool              hasWhere = false;
                            std::stringstream ss;
                            if ( canPartialIndex() && !whereClause.empty() ) {
                                hasWhere = true;
                                ss << "SELECT ( " << expression.asString() << " ) FROM _ WHERE ( "
                                   << whereClause.asString() << " )";
                                result = (MutableDict*)n1ql::parse(ss.str(), &errPos);
                            } else {
                                result = (MutableDict*)n1ql::parse(expression.asString(), &errPos);
                            }
                            if ( !result ) {
                                string errExpr = "Invalid N1QL in index expression \"";
                                if ( ss.peek() != EOF ) errExpr += ss.str();
                                else
                                    errExpr += expression.asString();
                                errExpr += "\"";
                                throw Query::parseError(errExpr.c_str(), errPos);
                            }
                            if ( hasWhere ) result->remove("FROM"_sl);
                            json = result->toJSON(true);
                            FLMutableDict_Release((FLMutableDict)result);
                        } else {
                            // n1ql parser won't compile empty string to empty array. Do it manually.
                            json = "[]";  // empty WHAT cannot be followed by WHERE clause.
                        }
                        _doc = Doc::fromJSON(json);
                    } catch ( const std::runtime_error& exc ) {
                        if ( dynamic_cast<const Query::parseError*>(&exc) ) throw;
                        else
                            error::_throw(error::InvalidQuery, "Invalid N1QL in index expression");
                    }
                    break;
            }
        }
        return _doc;
    }

    const Array* IndexSpec::what() const {
        const Array* what;
        if ( auto dict = doc()->asDict(); dict ) {
            what = qp::requiredArray(qp::getCaseInsensitive(dict, "WHAT"), "Index WHAT term");
        } else {
            // For backward compatibility, the JSON is allowed to be just an array
            // of expressions.
            what = qp::requiredArray(doc()->root(), "Index JSON");
        }
        return what;
    }

    const Array* IndexSpec::where() const {
        if ( auto dict = doc()->asDict(); dict ) {
            if ( auto whereVal = qp::getCaseInsensitive(dict, "WHERE"); whereVal )
                return qp::requiredArray(whereVal, "Index WHERE term");
        }
        return nullptr;
    }

    const Array* IndexSpec::unnestPaths() const {
        const ArrayOptions* arrayOpts = arrayOptions();
        if ( !arrayOpts || !arrayOpts->unnestPath )
            error::_throw(error::InvalidParameter, "IndexOptions for ArrayIndex must include unnestPath.");

        if ( auto dict = unnestDoc()->asDict(); dict ) {
            if ( auto whatVal = qp::getCaseInsensitive(dict, "WHAT"); whatVal )
                return qp::requiredArray(whatVal, "Index WHAT term");
        }
        return nullptr;
    }

    Doc* IndexSpec::unnestDoc() const {
        if ( !_unnestDoc ) {
            try {
                string_view unnestPath = arrayOptions()->unnestPath;
                // Split unnestPath from the options by "[]."
                std::vector<std::string_view> splitPaths;
                for ( size_t pos = 0; pos < unnestPath.length() && pos != string::npos; ) {
                    size_t next = unnestPath.find(KeyStore::kUnnestLevelSeparator, pos);
                    if ( next == string::npos ) {
                        splitPaths.push_back(unnestPath.substr(pos));
                        pos = string::npos;
                    } else {
                        splitPaths.push_back(unnestPath.substr(pos, next - pos));
                        pos = next + 3;
                    }
                }
                if ( splitPaths.empty() )
                    error::_throw(error::InvalidParameter,
                                  "IndexOptions for ArrayIndex must have non-empty unnestPath.");

                string n1qlUnnestPaths = string(splitPaths[0]);
                for ( unsigned i = 1; i < splitPaths.size(); ++i ) {
                    n1qlUnnestPaths += ", ";
                    n1qlUnnestPaths += splitPaths[i];
                }
                int           errPos;
                FLMutableDict result = n1ql::parse(n1qlUnnestPaths, &errPos);
                if ( !result ) {
                    string msg = "N1QL syntax error in unnestPath \"" + n1qlUnnestPaths + "\"";
                    throw Query::parseError(msg.c_str(), errPos);
                }

                alloc_slice json = ((MutableDict*)result)->toJSON(true);
                FLMutableDict_Release(result);
                _unnestDoc = Doc::fromJSON(json);
            } catch ( const std::runtime_error& exc ) {
                error::_throw(error::InvalidQuery, "Invalid N1QL in unnestPath (%s)", exc.what());
            }
        }
        return _unnestDoc;
    }

}  // namespace litecore
