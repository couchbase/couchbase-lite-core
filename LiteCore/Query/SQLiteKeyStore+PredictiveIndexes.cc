#ifdef COUCHBASE_ENTERPRISE

//
// SQLiteKeyStore+PredictiveIndexes.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#    include "SQLiteKeyStore.hh"
#    include "SQLiteDataFile.hh"
#    include "QueryParser.hh"
#    include "SQLUtil.hh"
#    include "Error.hh"
#    include "StringUtil.hh"
#    include "MutableArray.hh"
#    include "SQLiteCpp/SQLiteCpp.h"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    bool SQLiteKeyStore::createPredictiveIndex(const IndexSpec& spec) {
        auto expressions = spec.what();
        if ( expressions->count() != 1 )
            error::_throw(error::InvalidQuery, "Predictive index requires exactly one expression");
        const Array* expression = expressions->get(0)->asArray();
        if ( !expression ) error::_throw(error::InvalidQuery, "Predictive index requires a PREDICT() expression");

        // Create a table of the PREDICTION results:
        auto pred = MutableArray::newArray(expression);
        if ( pred->count() > 3 ) pred->remove(3, pred->count() - 3);
        string predTableName = createPredictionTable(pred, spec.optionsPtr());

        // The final parameters are the result properties to create a SQL index on:
        Array::iterator i(expression);
        i += 3;

        // If there are no result properties specified, skip creating the value index;
        // only the PREDICTION result table will be created and used as result cache.
        if ( !i ) {
            // Register the index to the indexes table without creating an actual index:
            db().createIndex(spec, this, predTableName, "");
            return true;
        }

        // Create value index on the specified result properties:
        return createIndex(spec, predTableName, i);
    }

    string SQLiteKeyStore::createPredictionTable(const Value* expression, const IndexSpec::Options* options) {
        // Derive the table name from the expression (path) it unnests:
        auto        kvTableName   = tableName();
        auto        q_kvTableName = quotedTableName();
        QueryParser qp(db(), "", kvTableName);
        auto        predTableName = qp.predictiveTableName(expression);

        // Create the index table, unless an identical one already exists:
        string sql = CONCAT("CREATE TABLE " << sqlIdentifier(predTableName)
                                            << " "
                                               "(docid INTEGER PRIMARY KEY REFERENCES "
                                            << q_kvTableName
                                            << "(rowid), "
                                               " body BLOB NOT NULL ON CONFLICT IGNORE) "
                                               "WITHOUT ROWID");
        if ( !db().schemaExistsWithSQL(predTableName, "table", predTableName, sql) ) {
            LogTo(QueryLog, "Creating predictive table '%s' on %s", predTableName.c_str(),
                  expression->toJSONString().c_str());
            db().exec(sql);

            // Populate the index-table with data from existing documents:
            string predictExpr = qp.expressionSQL(expression);
            db().exec(CONCAT("INSERT INTO " << sqlIdentifier(predTableName)
                                            << " (docid, body) "
                                               "SELECT rowid, "
                                            << predictExpr << "FROM " << q_kvTableName << " WHERE (flags & 1) = 0"));

            // Set up triggers to keep the index-table up to date
            // ...on insertion:
            qp.setBodyColumnName("new.body");
            predictExpr              = qp.expressionSQL(expression);
            string insertTriggerExpr = CONCAT("INSERT INTO " << sqlIdentifier(predTableName)
                                                             << " (docid, body) "
                                                                "VALUES (new.rowid, "
                                                             << predictExpr << ")");
            createTrigger(predTableName, "ins", "AFTER INSERT", "WHEN (new.flags & 1) = 0", insertTriggerExpr);

            // ...on delete:
            string deleteTriggerExpr = CONCAT("DELETE FROM " << sqlIdentifier(predTableName)
                                                             << " "
                                                                "WHERE docid = old.rowid");
            createTrigger(predTableName, "del", "BEFORE DELETE", "WHEN (old.flags & 1) = 0", deleteTriggerExpr);

            // ...on update:
            createTrigger(predTableName, "preupdate", "BEFORE UPDATE OF body, flags", "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);
            createTrigger(predTableName, "postupdate", "AFTER UPDATE OF body, flags", "WHEN (new.flags) & 1 = 0",
                          insertTriggerExpr);
        }
        return predTableName;
    }

}  // namespace litecore

#endif  // COUCHBASE_ENTERPRISE
