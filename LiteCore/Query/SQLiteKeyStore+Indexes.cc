//
// SQLiteKeyStore+Indexes.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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


#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Query.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "Stopwatch.hh"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    /*
     - A value index is a SQL index named 'NAME'.
     - A FTS index is a SQL virtual table named 'kv_default::NAME'
     - An array index has two parts:
         * A SQL table named `kv_default:unnest:PATH`, where PATH is the property path
         * An index on that table named `NAME`
     - A predictive index has two parts:
         * A SQL table named `kv_default:prediction:DIGEST`, where DIGEST is a unique digest
            of the prediction function name and the parameter dictionary
         * An index on that table named `NAME`

     Index table:
        - name (string primary key)
        - type (integer)
        - expression (JSON)
        - table name (string)
     The SQL index always is always named `name`.
     */

    static void validateIndexName(slice name);
    static pair<alloc_slice, const Array*> parseIndexExpr(slice expression, KeyStore::IndexType);


    bool SQLiteKeyStore::createIndex(const IndexSpec &spec,
                                     const IndexOptions *options) {
        validateIndexName(spec.name);
        alloc_slice expressionFleece;
        const Array *params;
        tie(expressionFleece, params) = parseIndexExpr(spec.expressionJSON, spec.type);

        Stopwatch st;
        Transaction t(db());
        bool created;
        switch (spec.type) {
            case kValueIndex: {
                Array::iterator iParams(params);
                created = createValueIndex(spec, tableName(), iParams, options);
                break;
            }
            case kFullTextIndex:  created = createFTSIndex(spec, params, options); break;
            case kArrayIndex:     created = createArrayIndex(spec, params, options); break;
#ifdef COUCHBASE_ENTERPRISE
            case kPredictiveIndex:created = createPredictiveIndex(spec, params, options); break;
#endif
            default:             error::_throw(error::Unimplemented);
        }

        if (created) {
            t.commit();
            db().optimize();
            double time = st.elapsed();
            QueryLog.log((time < 3.0 ? LogLevel::Info : LogLevel::Warning),
                         "Created index '%s' in %.3f sec", spec.name.c_str(), time);
        }
        return created;
    }


    void SQLiteKeyStore::deleteIndex(slice name)  {
        Transaction t(db());
        auto spec = db().getIndex(name);
        if (spec) {
            db().deleteIndex(spec);
            t.commit();
        } else {
            t.abort();
        }
    }


    // Creates the special by-sequence index
    void SQLiteKeyStore::createSequenceIndex() {
        if (!_createdSeqIndex) {
            Assert(_capabilities.sequences);
            db().execWithLock(CONCAT("CREATE UNIQUE INDEX IF NOT EXISTS kv_" << name() << "_seqs"
                                     " ON kv_" << name() << " (sequence)"));
            _createdSeqIndex = true;
        }
    }


    vector<KeyStore::IndexSpec> SQLiteKeyStore::getIndexes() const {
        vector<KeyStore::IndexSpec> result;
        for (auto &spec : db().getIndexes(nullptr)) {
            if (spec.keyStoreName == name())
                result.push_back(spec);
        }
        return result;
    }


#pragma mark - VALUE INDEX:


    // Creates a value index.
    bool SQLiteKeyStore::createValueIndex(const IndexSpec &spec,
                                          const string &sourceTableName,
                                          Array::iterator &expressions,
                                          const IndexOptions *options)
    {
        Assert(spec.type != kFullTextIndex);
        QueryParser qp(*this);
        qp.setTableName(CONCAT('"' << sourceTableName << '"'));
        qp.writeCreateIndex(spec.name, expressions, (spec.type != kValueIndex));
        string sql = qp.SQL();
        return db().createIndex(spec, this, sourceTableName, sql);
    }


#pragma mark - UTILITIES:


    // Part of the QueryParser delegate API
    bool SQLiteKeyStore::tableExists(const std::string &tableName) const {
        return db().tableExists(tableName);
    }


    static void validateIndexName(slice name) {
        if(name.size == 0) {
            error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not be empty");
        }
        if(name.findByte((uint8_t)'"') != nullptr) {
            error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not contain "
                          "the double quote (\") character");
        }
    }


    // Parses the JSON index-spec expression into an Array:
    static pair<alloc_slice, const Array*> parseIndexExpr(slice expression,
                                                          KeyStore::IndexType type)
    {
        alloc_slice expressionFleece;
        const Array *params = nullptr;
        try {
            Retained<Doc> doc = Doc::fromJSON(expression);
            expressionFleece = doc->allocedData();
            params = doc->asArray();
        } catch (const FleeceException &) { }
        if (!params || params->count() == 0)
            error::_throw(error::InvalidQuery, "JSON syntax error, or not an array");
        return {expressionFleece, params};
    }

}
