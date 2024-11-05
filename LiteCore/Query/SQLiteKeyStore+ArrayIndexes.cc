//
// SQLiteKeyStore+ArrayIndexes.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "QueryTranslator.hh"
#include "SecureDigest.hh"
#include "SQLUtil.hh"
#include "StringUtil.hh"
#include "Array.hh"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    bool SQLiteKeyStore::createArrayIndex(const IndexSpec& spec) {
        // the following will throw if !spec.arrayOptions() || !spec.arrayOpeionts()->unnestPath
        Array::iterator itPath(spec.unnestPaths());

        auto currSpec = db().getIndex(spec.name);
        if ( currSpec ) {
            // If there is already index with the index name,
            // eiher delete the current one, or use it (return false)
            while ( true ) {
                if ( currSpec->type != IndexSpec::kArray ) break;
                if ( !currSpec->arrayOptions() ) break;
                if ( currSpec->arrayOptions()->unnestPath != spec.arrayOptions()->unnestPath ) break;
                if ( !currSpec->what() || !spec.what() ) break;
                if ( currSpec->what()->toJSONString() != spec.what()->toJSONString() ) break;
                // Same index spec and unnestPath
                return false;
            }
            db().deleteIndex(*currSpec);
        }

        string plainTableName, unnestTableName;
        for ( ; itPath; ++itPath ) {
            std::tie(plainTableName, unnestTableName) =
                    createUnnestedTable(itPath.value(), plainTableName, unnestTableName);
        }
        Array::iterator iExprs((const Array*)spec.what());
        return createIndex(spec, plainTableName, iExprs);
    }

    std::pair<string, string> SQLiteKeyStore::createUnnestedTable(const Value* expression, string plainParentTable,
                                                                  string parentTable) {
        // Derive the table name from the expression it unnests:
        if ( plainParentTable.empty() ) plainParentTable = parentTable = tableName();
        QueryTranslator qp(db(), "", plainParentTable);
        string      plainTableName    = qp.unnestedTableName(expression);
        string      unnestTableName   = hexName(plainTableName);
        string      quotedParentTable = CONCAT(sqlIdentifier(parentTable));

        // Create the index table, unless an identical one already exists:
        string sql = CONCAT("CREATE TABLE " << sqlIdentifier(unnestTableName)
                                            << " "
                                               "(docid INTEGER NOT NULL REFERENCES "
                                            << sqlIdentifier(parentTable)
                                            << "(rowid), "
                                               " i INTEGER NOT NULL,"
                                               " body BLOB NOT NULL, "
                                               " CONSTRAINT pk PRIMARY KEY (docid, i))");
        if ( !db().schemaExistsWithSQL(unnestTableName, "table", unnestTableName, sql) ) {
            LogTo(QueryLog, "Creating UNNEST table '%s' on %s", unnestTableName.c_str(),
                  expression->toJSON(true).asString().c_str());
            db().exec(sql);

            qp.setBodyColumnName("new.body");
            string eachExpr = qp.eachExpressionSQL(FLValue(expression));
            bool   nested   = plainParentTable.find(KeyStore::kUnnestSeparator) != string::npos;

            // Populate the index-table with data from existing documents:
            if ( !nested ) {
                db().exec(CONCAT("INSERT INTO " << sqlIdentifier(unnestTableName)
                                                << " (docid, i, body) "
                                                   "SELECT new.rowid, _each.rowid, _each.value "
                                                << "FROM " << sqlIdentifier(parentTable) << " as new, " << eachExpr
                                                << " AS _each "
                                                   "WHERE (new.flags & 1) = 0"));
            } else {
                db().exec(CONCAT("INSERT INTO " << sqlIdentifier(unnestTableName)
                                                << " (docid, i, body) "
                                                   "SELECT new.rowid, _each.rowid, _each.value "
                                                << "FROM " << sqlIdentifier(parentTable) << " as new, " << eachExpr
                                                << " AS _each"));
            }

            // Set up triggers to keep the index-table up to date
            // ...on insertion:
            string insertTriggerExpr = CONCAT("INSERT INTO " << sqlIdentifier(unnestTableName)
                                                             << " (docid, i, body) "
                                                                "SELECT new.rowid, _each.rowid, _each.value "
                                                             << "FROM " << eachExpr << " AS _each ");
            // ...on delete:
            string deleteTriggerExpr = CONCAT("DELETE FROM " << sqlIdentifier(unnestTableName)
                                                             << " "
                                                                "WHERE docid = old.rowid");

            if ( !nested ) {
                createTrigger(unnestTableName, "ins", "AFTER INSERT", "WHEN (new.flags & 1) = 0", insertTriggerExpr,
                              quotedParentTable);
                createTrigger(unnestTableName, "del", "BEFORE DELETE", "WHEN (old.flags & 1) = 0", deleteTriggerExpr,
                              quotedParentTable);

                // ...on update:
                createTrigger(unnestTableName, "preupdate", "BEFORE UPDATE OF body, flags", "WHEN (old.flags & 1) = 0",
                              deleteTriggerExpr, quotedParentTable);
                createTrigger(unnestTableName, "postupdate", "AFTER UPDATE OF body, flags", "WHEN (new.flags & 1 = 0)",
                              insertTriggerExpr, quotedParentTable);
            } else {
                createTrigger(unnestTableName, "ins", "AFTER INSERT", "", insertTriggerExpr, quotedParentTable);
                createTrigger(unnestTableName, "del", "BEFORE DELETE", "", deleteTriggerExpr, quotedParentTable);
            }
        }
        return {plainTableName, unnestTableName};
    }

}  // namespace litecore
