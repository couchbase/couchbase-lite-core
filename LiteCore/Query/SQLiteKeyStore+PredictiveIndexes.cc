#ifdef COUCHBASE_ENTERPRISE

//
// SQLiteKeyStore+PredictiveIndexes.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
//  COUCHBASE LITE ENTERPRISE EDITION
//
//  Licensed under the Couchbase License Agreement (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//  https://info.couchbase.com/rs/302-GJY-034/images/2017-10-30_License_Agreement.pdf
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "MutableArray.hh"
#include "SQLiteCpp/SQLiteCpp.h"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    bool SQLiteKeyStore::createPredictiveIndex(const IndexSpec &spec) {
        auto expressions = spec.what();
        if (expressions->count() != 1)
            error::_throw(error::InvalidQuery, "Predictive index requires exactly one expression");
        const Array *expression = expressions->get(0)->asArray();
        if (!expression)
            error::_throw(error::InvalidQuery, "Predictive index requires a PREDICT() expression");

        // Create a table of the PREDICTION results:
        auto pred = MutableArray::newArray(expression);
        if (pred->count() > 3)
            pred->remove(3, pred->count() - 3);
        string predTableName = createPredictionTable(pred, spec.optionsPtr());

        // The final parameters are the result properties to create a SQL index on:
        Array::iterator i(expression);
        i += 3;
        
        // If there are no result properties specified, skip creating the value index;
        // only the PREDICTION result table will be created and used as result cache.
        if (!i) {
            // Register the index to the indexes table without creating an actual index:
            db().createIndex(spec, this, predTableName, "");
            return true;
        }
        
        // Create value index on the specified result properties:
        return createIndex(spec, predTableName, i);
    }


    string SQLiteKeyStore::createPredictionTable(const Value *expression,
                                                 const IndexSpec::Options *options)
    {
        // Derive the table name from the expression (path) it unnests:
        auto kvTableName = tableName();
        QueryParser qp(db(), kvTableName);
        auto predTableName = qp.predictiveTableName(expression);

        // Create the index table, unless an identical one already exists:
        string sql = CONCAT("CREATE TABLE \"" << predTableName << "\" "
                            "(docid INTEGER PRIMARY KEY REFERENCES " << kvTableName << "(rowid), "
                            " body BLOB NOT NULL ON CONFLICT IGNORE) "
                            "WITHOUT ROWID");
        if (!db().schemaExistsWithSQL(predTableName, "table", predTableName, sql)) {
            LogTo(QueryLog, "Creating predictive table '%s' on %s", predTableName.c_str(),
                  expression->toJSONString().c_str());
            db().exec(sql);

            // Populate the index-table with data from existing documents:
            string predictExpr = qp.expressionSQL(expression);
            db().exec(CONCAT("INSERT INTO \"" << predTableName << "\" (docid, body) "
                             "SELECT rowid, " << predictExpr <<
                             "FROM " << kvTableName << " WHERE (flags & 1) = 0"));

            // Set up triggers to keep the index-table up to date
            // ...on insertion:
            qp.setBodyColumnName("new.body");
            predictExpr = qp.expressionSQL(expression);
            string insertTriggerExpr = CONCAT("INSERT INTO \"" << predTableName <<
                                              "\" (docid, body) "
                                              "VALUES (new.rowid, " << predictExpr << ")");
            createTrigger(predTableName, "ins",
                          "AFTER INSERT",
                          "WHEN (new.flags & 1) = 0",
                          insertTriggerExpr);

            // ...on delete:
            string deleteTriggerExpr = CONCAT("DELETE FROM \"" << predTableName << "\" "
                                              "WHERE docid = old.rowid");
            createTrigger(predTableName, "del",
                          "BEFORE DELETE",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);

            // ...on update:
            createTrigger(predTableName, "preupdate",
                          "BEFORE UPDATE OF body, flags",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);
            createTrigger(predTableName, "postupdate",
                          "AFTER UPDATE OF body, flags",
                          "WHEN (new.flags) & 1 = 0",
                          insertTriggerExpr);
        }
        return predTableName;
    }

}

#endif // COUCHBASE_ENTERPRISE
