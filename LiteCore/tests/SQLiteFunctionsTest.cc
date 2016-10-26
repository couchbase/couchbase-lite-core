//
//  SQLiteFunctionsTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/10/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "LiteCoreTest.hh"
#include "SQLite_Internal.hh"
#include "Fleece.hh"
#include "SQLiteCpp/SQLiteCpp.h"

using namespace litecore;
using namespace fleece;
using namespace std;


// http://www.sqlite.org/json1.html#jeach


class SQLiteFunctionsTest {
public:

    static constexpr int numberOfOptions = 2;

    SQLiteFunctionsTest(int which)
    :db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
    {
        // Run test once with shared keys, once without:
        if (which & 1)
            sharedKeys.reset(new SharedKeys());
        RegisterFleeceFunctions(db.getHandle(), sharedKeys.get());
        RegisterFleeceEachFunctions(db.getHandle(), sharedKeys.get());
        db.exec("CREATE TABLE kv (key TEXT, body BLOB)");
        insertStmt.reset(new SQLite::Statement(db, "INSERT INTO kv (key, body) VALUES (?, ?)"));
    }

    void insert(const char *key, const char *json) {
        auto body = JSONConverter::convertJSON(slice(json), sharedKeys.get());
        insertStmt->bind(1, key);
        insertStmt->bind(2, body.buf, (int)body.size);
        insertStmt->exec();
        insertStmt->reset();
    }

    vector<string> query(const char *query) {
        SQLite::Statement each(db, query);
        vector<string> results;
        while (each.executeStep()) {
            results.push_back( each.getColumn(0) );
        }
        return results;
    }

protected:
    SQLite::Database db;
    unique_ptr<SQLite::Statement> insertStmt;
    std::unique_ptr<SharedKeys> sharedKeys;
};


N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_contains") {
    insert("one",   "{\"hey\": [1, 2, 3, 4]}");
    insert("two",   "{\"hey\": [2, 4, 6, 8]}");
    insert("three", "{\"hey\": [1, \"T\", 3.1416, []]}");
    insert("four",  "{\"hey\": [1, \"T\", 3.15,   []]}");
    insert("yerg",  "{\"xxx\": [1, \"T\", 3.1416, []]}");

    REQUIRE(query("SELECT key FROM kv WHERE fl_contains(kv.body, 'hey', 0, 4)")
            == (vector<string>{"one", "two"}));
    REQUIRE(query("SELECT key FROM kv WHERE fl_contains(kv.body, 'hey', 1, 3.1416, 'T')")
            == (vector<string>{"three"}));

}


N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_each array", "[fl_each]") {
    insert("one",   "[1, 2, 3, 4]");
    insert("two",   "[2, 4, 6, 8]");
    insert("three", "[3, 6, 9, \"dozen\"]");

    REQUIRE(query("SELECT fl_each.value FROM kv, fl_each(kv.body) WHERE kv.key = 'three'")
            == (vector<string>{"3", "6", "9", "dozen"}));
    REQUIRE(query("SELECT fl_each.key FROM kv, fl_each(kv.body) WHERE kv.key = 'three'")
            == (vector<string>{"", "", "", ""}));
    REQUIRE(query("SELECT fl_each.type FROM kv, fl_each(kv.body) WHERE kv.key = 'three'")
            == (vector<string>{"2", "2", "2", "3"}));
    REQUIRE(query("SELECT DISTINCT kv.key FROM kv, fl_each(kv.body) WHERE fl_each.value = 4")
            == (vector<string>{"one", "two"}));
}


N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_each dict", "[fl_each]") {
    insert("a",   "{\"one\": 1, \"two\": 2, \"three\": 3}");
    insert("b",   "{\"one\": 2, \"two\": 4, \"three\": 6}");
    insert("c",   "{\"one\": 3, \"two\": 6, \"three\": 9}");

    REQUIRE(query("SELECT fl_each.value FROM kv, fl_each(kv.body) WHERE kv.key = 'c' ORDER BY fl_each.value")
            == (vector<string>{"3", "6", "9"}));
    REQUIRE(query("SELECT fl_each.key FROM kv, fl_each(kv.body) WHERE kv.key = 'c' ORDER BY fl_each.key")
            == (vector<string>{"one", "three", "two"}));
    REQUIRE(query("SELECT fl_each.type FROM kv, fl_each(kv.body) WHERE kv.key = 'c'")
            == (vector<string>{"2", "2", "2"}));
    REQUIRE(query("SELECT DISTINCT kv.key FROM kv, fl_each(kv.body) WHERE fl_each.value = 2")
            == (vector<string>{"a", "b"}));
}


N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_each with path", "[fl_each]") {
    insert("one",   "{\"hey\": [1, 2, 3, 4]}");
    insert("two",   "{\"hey\": [2, 4, 6, 8]}");
    insert("three", "{\"xxx\": [1, 2, 3, 4]}");

    REQUIRE(query("SELECT fl_each.value FROM kv, fl_each(kv.body, 'hey') WHERE kv.key = 'one'")
            == (vector<string>{"1", "2", "3", "4"}));
    REQUIRE(query("SELECT fl_each.value FROM kv, fl_each(kv.body, 'hey') WHERE kv.key = 'three'")
            == (vector<string>{}));
    REQUIRE(query("SELECT DISTINCT kv.key FROM kv, fl_each(kv.body, 'hey') WHERE fl_each.value = 3")
            == (vector<string>{"one"}));
}
