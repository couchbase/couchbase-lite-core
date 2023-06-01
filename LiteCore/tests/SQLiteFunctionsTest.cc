//
// SQLiteFunctionsTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LiteCoreTest.hh"
#include "SQLite_Internal.hh"
#include "StringUtil.hh"
#include "UnicodeCollator.hh"
#include "FleeceImpl.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>

using namespace litecore;
using namespace fleece;
using namespace fleece::impl;
using namespace std;

// http://www.sqlite.org/json1.html#jeach


class SQLiteFunctionsTest
    : TestFixture
    , DataFile::Delegate {
  public:
    static constexpr int numberOfOptions = 2;

    explicit SQLiteFunctionsTest(int which) : db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {
        // Run test once with shared keys, once without:
        if ( which & 1 ) sharedKeys = new SharedKeys();
        RegisterSQLiteFunctions(db.getHandle(), {this, sharedKeys});
        db.exec("CREATE TABLE kv (key TEXT, body BLOB)");
        insertStmt = make_unique<SQLite::Statement>(db, "INSERT INTO kv (key, body) VALUES (?, ?)");
    }

    void insert(const char* key, const char* json) {
        auto body = JSONConverter::convertJSON(slice(json5(json)), sharedKeys);
        insertStmt->bind(1, key);
        insertStmt->bind(2, body.buf, (int)body.size);
        insertStmt->exec();
        insertStmt->reset();
    }

    [[nodiscard]] string databaseName() const override { return "db"; }

    alloc_slice blobAccessor(const Dict* blob) const override {
        auto digestProp = blob->get("digest"_sl);
        if ( !digestProp ) return {};
        slice digest = digestProp->asString();
        CHECK(digest);
        if ( digest.hasPrefix("sha1-"_sl) ) {
            return alloc_slice(digest.from(5));
        } else {
            return {};
        }
    }

    vector<string> query(const char* query) {
        SQLite::Statement each(db, query);
        vector<string>    results;
        while ( each.executeStep() ) {
            auto column = each.getColumn(0);
            if ( column.getType() == SQLITE_NULL ) {
                results.emplace_back("MISSING");
            } else if ( column.getType() == SQLITE_BLOB && column.getBytes() == 0 ) {
                results.emplace_back("null");
            } else {
                results.emplace_back(each.getColumn(0).getText());
            }
        }
        return results;
    }

    vector<string> query(const string& query) { return this->query(query.c_str()); }

  protected:
    SQLite::Database              db;
    unique_ptr<SQLite::Statement> insertStmt;
    Retained<SharedKeys>          sharedKeys;
};

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_contains", "[Query]") {
    // fl_contains is called for the ANY operator when the condition is a simple equality test
    insert("one", "{\"hey\": [1, 2, 3, 4]}");
    insert("two", "{\"hey\": [2, 4, 6, 8]}");
    insert("three", R"({"hey": [1, "T", "4", []]})");
    insert("four", R"({"hey": [1, "T", 3.15,   []]})");
    insert("five", R"({"hey": {"a": "bar", "b": 4}})");  // ANY supports dicts!
    insert("xorp", R"({"hey": "oops"})");
    insert("yerg", R"({"xxx": [1, "T", 3.1416, []]})");

    CHECK(query("SELECT key FROM kv WHERE fl_contains(kv.body, 'hey', 4)") == (vector<string>{"one", "two", "five"}));
    CHECK(query("SELECT key FROM kv WHERE fl_contains(kv.body, 'hey', 'T')") == (vector<string>{"three", "four"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite array_sum of fl_value", "[Query]") {
    insert("a", "{\"hey\": [1, 2, 3, 4]}");
    insert("b", "{\"hey\": [2, 4, 6, 8]}");
    insert("c", "{\"hey\": []}");
    insert("d", R"({"hey": [1, 2, true, "foo"]})");
    insert("e", "{\"xxx\": [1, 2, 3, 4]}");

    CHECK(query("SELECT ARRAY_SUM(fl_value(body, 'hey')) FROM kv")
          == (vector<string>{"10.0", "20.0", "0.0", "4.0", "0.0"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite array_avg of fl_value", "[Query]") {
    insert("a", "{\"hey\": [1, 2, 3, 4]}");
    insert("b", "{\"hey\": [2, 4, 6, 8]}");
    insert("c", "{\"hey\": []}");
    insert("d", R"({"hey": [1, 2, true, "foo"]})");
    insert("e", "{\"xxx\": [1, 2, 3, 4]}");

    CHECK(query("SELECT ARRAY_AVG(fl_value(body, 'hey')) FROM kv")
          == (vector<string>{"2.5", "5.0", "0.0", "1.0", "0.0"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite array_contains of fl_value", "[Query]") {
    insert("a", R"({"hey": [1, 1, 2, true, true, 4, "bar"]})");
    insert("b", "{\"hey\": [1, 1, 2, true, true, 4]}");
    insert("c", R"({"hey": [1, 1, 2, "4", "bar"]})");
    insert("e", R"({"hey": {"a": "bar", "b": 1}})");  // array_contains doesn't match dicts!
    insert("f", R"({"hey": "bar"})");
    insert("d", R"({"xxx": [1, 1, 2, "bar"]})");
    insert("g", "{\"hey\": [true, 0]}");
    insert("h", "{\"hey\": [false, 1]}");

    CHECK(query("SELECT ARRAY_CONTAINS(fl_value(body, 'hey'), 4) FROM kv")
          == (vector<string>{"1", "1", "0", "null", "null", "MISSING", "0", "0"}));
    CHECK(query("SELECT ARRAY_CONTAINS(fl_value(body, 'hey'), 'bar') FROM kv")
          == (vector<string>{"1", "0", "1", "null", "null", "MISSING", "0", "0"}));
    CHECK(query("SELECT ARRAY_CONTAINS(fl_value(body, 'hey'), fl_bool(1)) FROM kv")
          == vector<string>{"1", "1", "1", "null", "null", "MISSING", "1", "1"});
    CHECK(query("SELECT ARRAY_CONTAINS(fl_value(body, 'hey'), fl_bool(0)) FROM kv")
          == vector<string>{"0", "0", "0", "null", "null", "MISSING", "1", "1"});

    // select array_contains([3==3], true) => true
    CHECK(query("select array_contains(array_of(3 == 3), fl_bool(3)) from kv where kv.key = 'a'")
          == vector<string>{"1"});
    // select array_contains([4==3], true) => false
    CHECK(query("select array_contains(array_of(4 == 3), fl_bool(1)) from kv where kv.key = 'a'")
          == vector<string>{"0"});
    // select array_contains([3==3], false) => false
    CHECK(query("select array_contains(array_of(3 == 3), fl_bool(0)) from kv where kv.key = 'a'")
          == vector<string>{"0"});
    // select array_contains([4==3], false) => true
    CHECK(query("select array_contains(array_of(4 == 3), fl_bool(0)) from kv where kv.key = 'a'")
          == vector<string>{"1"});
    // select array_contains([1], true) => true
    CHECK(query("select array_contains(array_of(1), fl_bool(1)) from kv where kv.key = 'a'") == vector<string>{"1"});
    // select array_contains([2], true) => false
    CHECK(query("select array_contains(array_of(2), fl_bool(2)) from kv where kv.key = 'a'") == vector<string>{"0"});
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite array_ifnull of fl_value", "[Query]") {
    insert("a", R"({"hey": [null, null, 2, true, true, 4, "bar"]})");

    CHECK(query("SELECT ARRAY_IFNULL(fl_value(body, 'hey')) FROM kv") == (vector<string>{"2"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite array_min_max of fl_value", "[Query]") {
    insert("a", R"({"hey": [1, 4, 3, -50, 10, 4, "bar"]})");

    CHECK(query("SELECT ARRAY_MAX(fl_value(body, 'hey')) FROM kv") == (vector<string>{"10.0"}));
    CHECK(query("SELECT ARRAY_MIN(fl_value(body, 'hey')) FROM kv") == (vector<string>{"-50.0"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite array_agg", "[Query]") {
    insert("a", "{\"hey\": 17}");
    insert("b", "{\"hey\": 8.125}");
    insert("c", R"({"hey": "there"})");
    insert("c", "{\"hey\": null}");
    insert("d", "{}");
    insert("e", R"({"hey": [99, -5.5, "wow"]})");
    insert("f", "{\"hey\": 8.125}");

    const char* sql          = "SELECT ARRAY_AGG(fl_value(body, 'hey')) FROM kv";
    slice       expectedJSON = R"([17,8.125,"there",null,[99,-5.5,"wow"],8.125])";
    SECTION("Distinct") {
        sql          = "SELECT ARRAY_AGG(DISTINCT fl_value(body, 'hey')) FROM kv";
        expectedJSON = R"([17,8.125,"there",null,[99,-5.5,"wow"]])";
    }
    SQLite::Statement st(db, sql);
    REQUIRE(st.executeStep());
    auto column = st.getColumn(0);
    REQUIRE(column.getType() == SQLITE_BLOB);
    auto array = Value::fromData({column.getBlob(), (size_t)column.getBytes()});
    REQUIRE(array);
    // Note: Ordering is "arbitrary" according to SQLite docs, so it isn't required to be in the
    // order in this CHECK, though in practice it is.
    CHECK(array->toJSON() == expectedJSON);
    CHECK(!st.executeStep());
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "N1QL missingif/nullif", "[Query]") {
    insert("a", R"({"hey": [null, null, 2, true, true, 4, "bar"]})");

    CHECK(query("SELECT MISSINGIF('5', '5') FROM kv") == (vector<string>{"MISSING"}));
    CHECK(query("SELECT MISSINGIF(5, 5.0) FROM kv")  // compare int with float
          == (vector<string>{"MISSING"}));
    CHECK(query("SELECT MISSINGIF(9223372036854775807, 9.22337e+18) FROM kv")
          == (vector<string>{"9223372036854775807"}));
    CHECK(query("SELECT MISSINGIF(9223370000000000000, 9.22337e+18) FROM kv") == (vector<string>{"MISSING"}));
    CHECK(query("SELECT MISSINGIF(9.22337e+200, 9.22337e+200) FROM kv") == (vector<string>{"MISSING"}));
    CHECK(query("SELECT MISSINGIF(9223370000000000001, 9.22337e+18) FROM kv")
          == (vector<string>{"9223370000000000001"}));
    CHECK(query("SELECT MISSINGIF(9223372036854775807, 9223372036854775807) FROM kv") == (vector<string>{"MISSING"}));
    CHECK(query("SELECT N1QL_NULLIF(-9223372036854775808, -9223372036854775808) FROM kv") == (vector<string>{"null"}));
    CHECK(query("SELECT MISSINGIF('5', 5) FROM kv") == (vector<string>{"5"}));
    CHECK(query("SELECT MISSINGIF('5', '4') FROM kv") == (vector<string>{"5"}));
    CHECK(query("SELECT N1QL_NULLIF('5', '5') FROM kv") == (vector<string>{"null"}));
    CHECK(query("SELECT N1QL_NULLIF(5, '5') FROM kv") == (vector<string>{"5"}));
    CHECK(query("SELECT N1QL_NULLIF('5', '4') FROM kv") == (vector<string>{"5"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_each array", "[Query][fl_each]") {
    insert("one", "{array:[1, 2, 3, 4]}");
    insert("two", "{array:[2, 4, 6, 8]}");
    insert("three", "{array:[3, 6, 9, \"dozen\"]}");

    CHECK(query("SELECT fl_each.value FROM kv, fl_each(kv.body, 'array') WHERE kv.key = 'three'")
          == (vector<string>{"3", "6", "9", "dozen"}));
    CHECK(query("SELECT fl_each.key FROM kv, fl_each(kv.body, 'array') WHERE kv.key = 'three'")
          == (vector<string>{"MISSING", "MISSING", "MISSING", "MISSING"}));
    CHECK(query("SELECT fl_each.type FROM kv, fl_each(kv.body, 'array') WHERE kv.key = 'three'")
          == (vector<string>{"2", "2", "2", "3"}));
    CHECK(query("SELECT DISTINCT kv.key FROM kv, fl_each(kv.body, 'array') WHERE fl_each.value = 4")
          == (vector<string>{"one", "two"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_each dict", "[Query][fl_each]") {
    insert("a", R"({"one": 1, "two": 2, "three": 3})");
    insert("b", R"({"one": 2, "two": 4, "three": 6})");
    insert("c", R"({"one": 3, "two": 6, "three": 9})");

    CHECK(query("SELECT fl_each.value FROM kv, fl_each(kv.body, '.') WHERE kv.key = 'c' ORDER BY fl_each.value")
          == (vector<string>{"3", "6", "9"}));
    CHECK(query("SELECT fl_each.key FROM kv, fl_each(kv.body, '.') WHERE kv.key = 'c' ORDER BY fl_each.key")
          == (vector<string>{"one", "three", "two"}));
    CHECK(query("SELECT fl_each.type FROM kv, fl_each(kv.body, '.') WHERE kv.key = 'c'")
          == (vector<string>{"2", "2", "2"}));
    CHECK(query("SELECT DISTINCT kv.key FROM kv, fl_each(kv.body, '.') WHERE fl_each.value = 2")
          == (vector<string>{"a", "b"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_each with path", "[Query][fl_each]") {
    insert("one", "{\"hey\": [1, 2, 3, 4]}");
    insert("two", "{\"hey\": [2, 4, 6, 8]}");
    insert("three", "{\"xxx\": [1, 2, 3, 4]}");

    CHECK(query("SELECT fl_each.value FROM kv, fl_each(kv.body, 'hey') WHERE kv.key = 'one'")
          == (vector<string>{"1", "2", "3", "4"}));
    CHECK(query("SELECT fl_each.value FROM kv, fl_each(kv.body, 'hey') WHERE kv.key = 'three'").empty());
    CHECK(query("SELECT DISTINCT kv.key FROM kv, fl_each(kv.body, 'hey') WHERE fl_each.value = 3")
          == (vector<string>{"one"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite numeric ops", "[Query]") {
    insert("one", "{\"hey\": 4.0}");
    insert("one", "{\"hey\": 2.5}");


    CHECK(query("SELECT sqrt(fl_value(kv.body, 'hey')) FROM kv") == (vector<string>{"2.0", "1.58113883008419"}));
    CHECK(query("SELECT log(fl_value(kv.body, 'hey')) FROM kv")
          == (vector<string>{"0.602059991327962", "0.397940008672038"}));
    CHECK(query("SELECT ln(fl_value(kv.body, 'hey')) FROM kv")
          == (vector<string>{"1.38629436111989", "0.916290731874155"}));
    CHECK(query("SELECT exp(fl_value(kv.body, 'hey')) FROM kv")
          == (vector<string>{"54.5981500331442", "12.1824939607035"}));
    CHECK(query("SELECT power(fl_value(kv.body, 'hey'), 3) FROM kv") == (vector<string>{"64.0", "15.625"}));
    CHECK(query("SELECT floor(fl_value(kv.body, 'hey')) FROM kv") == (vector<string>{"4.0", "2.0"}));
    CHECK(query("SELECT ceil(fl_value(kv.body, 'hey')) FROM kv") == (vector<string>{"4.0", "3.0"}));
    CHECK(query("SELECT round(fl_value(kv.body, 'hey')) FROM kv") == (vector<string>{"4.0", "3.0"}));
    CHECK(query("SELECT round(fl_value(kv.body, 'hey'), 1) FROM kv") == (vector<string>{"4.0", "2.5"}));
    CHECK(query("SELECT trunc(fl_value(kv.body, 'hey')) FROM kv") == (vector<string>{"4.0", "2.0"}));
    CHECK(query("SELECT trunc(fl_value(kv.body, 'hey'), 1) FROM kv") == (vector<string>{"4.0", "2.5"}));
}

static void testTrim(const char16_t* str, int onSide, int leftTrimmed, int rightTrimmed) {
    auto   newStr = str;
    size_t length = 0;
    for ( auto cp = str; *cp; ++cp ) ++length;
    size_t newLength = length;

    UTF16Trim(newStr, newLength, onSide);
    CHECK(newStr - str == leftTrimmed);
    CHECK(newLength == max(0l, (long)length - leftTrimmed - rightTrimmed));
}

static void testTrim(const char16_t* str, int leftTrimmed, int rightTrimmed) {
    testTrim(str, -1, leftTrimmed, 0);
    testTrim(str, 0, leftTrimmed, rightTrimmed);
    testTrim(str, 1, 0, rightTrimmed);
}

TEST_CASE("Unicode string functions", "[Query]") {
    CHECK(UTF8Length(""_sl) == 0);
    CHECK(UTF8Length("x"_sl) == 1);
    CHECK(UTF8Length("xy"_sl) == 2);
    CHECK(UTF8Length("cafés"_sl) == 5);
    CHECK(UTF8Length("“÷”"_sl) == 3);
    CHECK(UTF8Length("😀"_sl) == 1);

    CHECK(UTF8ChangeCase(""_sl, true) == ""_sl);
    CHECK(UTF8ChangeCase("e"_sl, true) == "E"_sl);
    CHECK(UTF8ChangeCase("E"_sl, true) == "E"_sl);
    CHECK(UTF8ChangeCase("-"_sl, true) == "-"_sl);
    CHECK(UTF8ChangeCase("Z•rGMai2"_sl, true) == "Z•RGMAI2"_sl);
#if __APPLE__ || defined(_MSC_VER) || LITECORE_USES_ICU
    CHECK(UTF8ChangeCase("Zérgmåī2"_sl, true) == "ZÉRGMÅĪ2"_sl);
#endif
    CHECK(UTF8ChangeCase("😀"_sl, true) == "😀"_sl);

    CHECK(UTF8ChangeCase(""_sl, false) == ""_sl);
    CHECK(UTF8ChangeCase("E"_sl, false) == "e"_sl);
    CHECK(UTF8ChangeCase("e"_sl, false) == "e"_sl);
    CHECK(UTF8ChangeCase("-"_sl, false) == "-"_sl);
    CHECK(UTF8ChangeCase("Z•rGMai2"_sl, false) == "z•rgmai2"_sl);
#if __APPLE__ || defined(_MSC_VER) || LITECORE_USES_ICU
    CHECK(UTF8ChangeCase("zÉRGMÅĪ2"_sl, false) == "zérgmåī2"_sl);
#endif
    CHECK(UTF8ChangeCase("😀"_sl, false) == "😀"_sl);

    testTrim(u"", 0, 0);
    testTrim(u"x", 0, 0);
    testTrim(u" x", 1, 0);
    testTrim(u"x ", 0, 1);
    testTrim(u" x ", 1, 1);
    testTrim(u"   ", 3, 3);
    testTrim(u"\n stuff goes here\r\t", 2, 2);
    testTrim(u"\n stuff goes here\r\t", 2, 2);
    testTrim(u"\u1680\u180e\u2000\u2007\u200a", 3, 1);
    testTrim(u"\u2028\u2029\u2030\u205f\u3000", 2, 2);
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "N1QL string functions", "[Query]") {
    CHECK(query("SELECT N1QL_length('')") == (vector<string>{"0"}));
    CHECK(query("SELECT N1QL_length('12345')") == (vector<string>{"5"}));
    CHECK(query("SELECT N1QL_length('cafés')") == (vector<string>{"5"}));

    CHECK(query("SELECT N1QL_lower('cAFES17•')") == (vector<string>{"cafes17•"}));
    CHECK(query("SELECT N1QL_upper('cafes17')") == (vector<string>{"CAFES17"}));
#if __APPLE__ || defined(_MSC_VER) || LITECORE_USES_ICU
    CHECK(query("SELECT N1QL_lower('cAFÉS17•')") == (vector<string>{"cafés17•"}));
    CHECK(query("SELECT N1QL_upper('cafés17')") == (vector<string>{"CAFÉS17"}));
#endif
    CHECK(query("SELECT N1QL_ltrim('  x  ')") == (vector<string>{"x  "}));
    CHECK(query("SELECT N1QL_rtrim('  x  ')") == (vector<string>{"  x"}));
    CHECK(query("SELECT N1QL_trim('  x  ')") == (vector<string>{"x"}));
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite fl_blob", "[Query]") {
    insert("1", "{attachment: {digest: 'sha1-foobar', content_type: 'text/plain'}}");
    insert("2", "{attachment: {digest: 'sha1-bazz'}}");
    insert("3", "{attachment: {digest: 'rot13-whoops'}}");
    insert("4", "{attachment: {stub: true}}");
    insert("4", "{duh: false}");

    CHECK(query("SELECT fl_blob(body, '.attachment') FROM kv ORDER BY key")
          == (vector<string>{"foobar", "bazz", "MISSING", "MISSING", "MISSING"}));
}

#pragma mark - COLLATION:


#if __APPLE__ || defined(_MSC_VER) || LITECORE_USES_ICU
TEST_CASE("Unicode collation", "[Query][Collation]") {
    struct {
        slice a;
        slice b;
        int   result{};
        bool  caseSensitive{};
        bool  diacriticSensitive{};
    } tests[] = {//---- First, test just ASCII:

                 // Edge cases: empty and 1-char strings
                 {""_sl, ""_sl, 0, true, true},
                 {""_sl, "a"_sl, -1, true, true},
                 {"a"_sl, "a"_sl, 0, true, true},

                 // Case sensitive: lowercase comes first by Unicode rules
                 {"a"_sl, "A"_sl, -1, true, true},
                 {"abc"_sl, "abc"_sl, 0, true, true},
                 {"Aaa"_sl, "abc"_sl, -1, true, true},  // Because 'a'-vs-'b' beats 'A'-vs-'a'
                 {"abc"_sl, "abC"_sl, -1, true, true},
                 {"AB"_sl, "abc"_sl, -1, true, true},

                 // Case insensitive:
                 {"ABCDEF"_sl, "ZYXWVU"_sl, -1, false, true},
                 {"ABCDEF"_sl, "Z"_sl, -1, false, true},

                 {"a"_sl, "A"_sl, 0, false, true},
                 {"abc"_sl, "ABC"_sl, 0, false, true},
                 {"ABA"_sl, "abc"_sl, -1, false, true},

                 {"commonprefix1"_sl, "commonprefix2"_sl, -1, false, true},
                 {"commonPrefix1"_sl, "commonprefix2"_sl, -1, false, true},

                 {"abcdef"_sl, "abcdefghijklm"_sl, -1, false, true},
                 {"abcdeF"_sl, "abcdefghijklm"_sl, -1, false, true},

                 //---- Now bring in non-ASCII characters:

                 {"a"_sl, "á"_sl, -1, false, true},
                 {""_sl, "á"_sl, -1, false, true},
                 {"á"_sl, "á"_sl, 0, false, true},
                 {"•a"_sl, "•A"_sl, 0, false, true},

                 {"test a"_sl, "test á"_sl, -1, false, true},
                 {"test á"_sl, "test b"_sl, -1, false, true},
                 {"test á"_sl, "test Á"_sl, 0, false, true},
                 {"test á1"_sl, "test Á2"_sl, -1, false, true},

                 // Case sensitive, diacritic sensitive:
                 {"ABCDEF"_sl, "ZYXWVU"_sl, -1, true, true},
                 {"ABCDEF"_sl, "Z"_sl, -1, true, true},
                 {"a"_sl, "A"_sl, -1, true, true},
                 {"abc"_sl, "ABC"_sl, -1, true, true},
                 {"•a"_sl, "•A"_sl, -1, true, true},
                 {"test a"_sl, "test á"_sl, -1, true, true},
                 {"Ähnlichkeit"_sl, "apple"_sl, -1, true, true},  // Because 'h'-vs-'p' beats 'Ä'-vs-'a'
                 {"ax"_sl, "Äz"_sl, -1, true, true},
                 {"test a"_sl, "test Á"_sl, -1, true, true},
                 {"test Á"_sl, "test e"_sl, -1, true, true},
                 {"test á"_sl, "test Á"_sl, -1, true, true},
                 {"test á"_sl, "test b"_sl, -1, true, true},
                 {"test u"_sl, "test Ü"_sl, -1, true, true},

                 // Case sensitive, diacritic insensitive:
                 {"abc"_sl, "ABC"_sl, -1, true, false},
                 {"test á"_sl, "test a"_sl, 0, true, false},
                 {"test á"_sl, "test A"_sl, -1, true, false},
                 {"test á"_sl, "test b"_sl, -1, true, false},
                 {"test á"_sl, "test Á"_sl, -1, true, false},

                 // Case and diacritic insensitive:
                 {"test á"_sl, "test Á"_sl, 0, false, false},

                 {}};

    for ( auto test = &tests[0]; test->a; ++test ) {
        Collation coll;
        coll.unicodeAware       = true;
        coll.caseSensitive      = test->caseSensitive;
        coll.diacriticSensitive = test->diacriticSensitive;
        INFO("Comparing '" << test->a.asString() << "', '" << test->b.asString() << "' (casesens="
                           << test->caseSensitive << ", diacsens=" << test->diacriticSensitive << ")");
        CHECK(CompareUTF8(test->a, test->b, coll) == test->result);

        INFO("Comparing '" << test->b.asString() << "' , '" << test->a.asString()
                           << "' casesens=" << test->caseSensitive << ", diacsens=" << test->diacriticSensitive << ")");
        CHECK(CompareUTF8(test->b, test->a, coll) == -test->result);
    }
}

TEST_CASE("Unicode locale collation", "[Query][Collation]") {
    // By default, "Å" sorts between "A" and "B"
    Collation coll;
    coll.unicodeAware = true;
    CHECK(CompareUTF8("Å"_sl, "A"_sl, coll) == 1);
    CHECK(CompareUTF8("Å"_sl, "B"_sl, coll) == -1);
    CHECK(CompareUTF8("Å"_sl, "Z"_sl, coll) == -1);

    CHECK(CompareUTF8("ch"_sl, "c"_sl, coll) == 1);
    CHECK(CompareUTF8("ch"_sl, "cz"_sl, coll) == -1);

    // But in Swedish, it comes after "Z"
    coll.localeName = "se"_sl;
    CHECK(CompareUTF8("Å"_sl, "A"_sl, coll) == 1);
    CHECK(CompareUTF8("Å"_sl, "B"_sl, coll) == 1);
    CHECK(CompareUTF8("Å"_sl, "Z"_sl, coll) == 1);
}

N_WAY_TEST_CASE_METHOD(SQLiteFunctionsTest, "SQLite collation", "[Query][Collation]") {
    CollationContextVector contexts;
    RegisterSQLiteUnicodeCollations(db.getHandle(), contexts);
    insert("a", R"({"hey": "Apple"})");
    insert("b", R"({"hey": "Aardvark"})");
    insert("c", "{\"hey\": \"Ångström\"}");
    insert("d", R"({"hey": "Zebra"})");
    insert("d", "{\"hey\": \"äpple\"}");

    {
        INFO("BINARY collation");
        CHECK(query(string("SELECT fl_value(body, 'hey') FROM kv ORDER BY fl_value(body, 'hey')") + "COLLATE "
                    + Collation(true).sqliteName())
              == (vector<string>{"Aardvark", "Apple", "Zebra", "Ångström", "äpple"}));
    }
    {
        INFO("NOCASE collation");
        CHECK(query(string("SELECT fl_value(body, 'hey') FROM kv ORDER BY fl_value(body, 'hey')") + "COLLATE "
                    + Collation(false).sqliteName())
              == (vector<string>{"Aardvark", "Apple", "Zebra", "Ångström", "äpple"}));
    }
    {
        INFO("Unicode case-sensitive, diacritic-sensitive collation");
        CHECK(query(string("SELECT fl_value(body, 'hey') FROM kv ORDER BY fl_value(body, 'hey')") + "COLLATE "
                    + Collation(true, true, nullslice).sqliteName())
              == (vector<string>{"Aardvark", "Ångström", "Apple", "äpple", "Zebra"}));
    }
    {
        INFO("Unicode case-INsensitive, diacritic-sensitive collation");
        CHECK(query(string("SELECT fl_value(body, 'hey') FROM kv ORDER BY fl_value(body, 'hey')") + "COLLATE "
                    + Collation(false, true, nullslice).sqliteName())
              == (vector<string>{"Aardvark", "Ångström", "Apple", "äpple", "Zebra"}));
    }
    {
        INFO("Unicode case-sensitive, diacritic-INsensitive collation");
        CHECK(query(string("SELECT fl_value(body, 'hey') FROM kv ORDER BY fl_value(body, 'hey')") + "COLLATE "
                    + Collation(true, false, nullslice).sqliteName())
              == (vector<string>{"Aardvark", "Ångström", "äpple", "Apple", "Zebra"}));
    }
    {
        INFO("Unicode case-INsensitive, diacritic-INsensitive collation");
        CHECK(query(string("SELECT fl_value(body, 'hey') FROM kv ORDER BY fl_value(body, 'hey')") + "COLLATE "
                    + Collation(false, false, nullslice).sqliteName())
              == (vector<string>{"Aardvark", "Ångström", "Apple", "äpple", "Zebra"}));
    }
}
#endif  //__APPLE__ || defined(_MSC_VER) || LITECORE_USES_ICU
