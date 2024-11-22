//
// Test_Translator.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "QueryTranslatorTest.hh"
#include "SQLiteDataFile.hh"
#include "SQLiteKeyStore.hh"
#include "ExprNodes.hh"
#include "SecureDigest.hh"
#include "SelectNodes.hh"
#include "StringUtil.hh"
#include <iosfwd>

using namespace std;
using namespace litecore::qt;
using namespace fleece;

inline string operator""_j5(const char* str, size_t len) { return json5({str, len}); }

string QueryTranslatorTest::parseWhere(string_view json) {
#if 1
    Log("### %.*s", int(json.size()), json.data());
    QueryTranslator t(*this, "_default", "kv_default");
    FLError         err;
    Doc             doc = Doc::fromJSON(json5(json), &err);
    REQUIRE(doc);
    string sql     = t.expressionSQL(doc.root());
    usedTableNames = t.collectionTablesUsed();
    return sql;
#else
    string                       sql     = parse("['SELECT', {WHERE: " + string(json) + "}]");
    static constexpr string_view kPrefix = "SELECT _doc.key, _doc.sequence FROM kv_default AS _doc WHERE ";
    REQUIRE(hasPrefix(sql, kPrefix));
    return sql.substr(kPrefix.size());
#endif
}

string QueryTranslatorTest::parse(string_view json) {
    Log("### %.*s", int(json.size()), json.data());
    QueryTranslator t(*this, "_default", "kv_default");
    t.parseJSON(json5(json));
    usedTableNames = t.collectionTablesUsed();
    return t.SQL();
}

string QueryTranslatorTest::parse(FLValue root) {
    usedTableNames.clear();
    QueryTranslator t(*this, "_default", "kv_default");
    t.parse(root);
    usedTableNames = t.collectionTablesUsed();
    return t.SQL();
}

void QueryTranslatorTest::mustFail(string_view json) {
    ExpectException(error::LiteCore, error::InvalidQuery, [&] { (void)parseWhere(json); });
}

[[nodiscard]] bool QueryTranslatorTest::tableExists(const string& tableName) const {
    string name = tableName;
    if ( hasPrefix(name, "all_") ) name = "kv_" + name.substr(4);
    bool exists = tableNames.count(name) > 0;
    Log("    tableExists(\"%s\") -> %s", tableName.c_str(), (exists ? "true" : "false"));
    return exists;
}

// Basically copied from SQLiteDataFile::collectionTableName()
[[nodiscard]] string QueryTranslatorTest::collectionTableName(const string& collection, DeletionStatus type) const {
    static const string kDeletedKeyStorePrefix = "del_";

    CHECK(!slice(collection).hasPrefix("kv_"));  // make sure I didn't get passed a table name
    string name;
    if ( type == QueryTranslator::kLiveAndDeletedDocs ) {
        name = "all_";
    } else {
        name = "kv_";
        if ( type == QueryTranslator::kDeletedDocs ) name += kDeletedKeyStorePrefix;
    }

    auto [scope, coll] = DataFile::splitCollectionPath(collection);

    if ( collection == "_" || (DataFile::isDefaultScope(scope) && DataFile::isDefaultCollection(coll)) ) {
        name += DataFile::kDefaultKeyStoreName;
    } else if ( !scope && coll == databaseName
                && !tableExists(name + string(KeyStore::kCollectionPrefix) + coll.asString()) ) {
        // The name of this database represents the default collection,
        // _unless_ there is a collection with that name.
        name += DataFile::kDefaultKeyStoreName;
    } else {
        string candidate = name + string(KeyStore::kCollectionPrefix);
        bool   isValid   = true;
        if ( !DataFile::isDefaultScope(scope) ) {
            if ( !KeyStore::isValidCollectionName(scope) ) {
                isValid = false;
            } else {
                candidate += SQLiteKeyStore::transformCollectionName(scope.asString(), true)
                             + KeyStore::kScopeCollectionSeparator;
            }
        }
        if ( isValid && KeyStore::isValidCollectionName(coll) ) {
            candidate += SQLiteKeyStore::transformCollectionName(coll.asString(), true);
        } else {
            error::_throw(error::InvalidQuery, "\"%s\" is not a valid collection name", collection.c_str());
        }
        name = candidate;
    }
    Log("    collectionTableName(\"%s\", %d) -> %s", collection.c_str(), int(type), name.c_str());
    return name;
}

string QueryTranslatorTest::FTSTableName(const string& onTable, const string& property) const {
    return SQLiteDataFile::auxiliaryTableName(onTable, KeyStore::kIndexSeparator, property);
}

string QueryTranslatorTest::unnestedTableName(const string& onTable, const string& property) const {
    return SQLiteDataFile::auxiliaryTableName(onTable, KeyStore::kUnnestSeparator, property);
}

#ifdef COUCHBASE_ENTERPRISE
string QueryTranslatorTest::predictiveTableName(const string& onTable, const string& property) const {
    return SQLiteDataFile::auxiliaryTableName(onTable, KeyStore::kPredictSeparator, property);
}

[[nodiscard]] string QueryTranslatorTest::vectorTableName(const string& onTable, const std::string& property,
                                                          string_view metricName) const {
    auto i = vectorIndexedProperties.find({onTable, property});
    if ( i == vectorIndexedProperties.end() )
        FAIL("there is no vector index of expression " + property + " on table " + onTable);
    string tableName = i->second;
    REQUIRE(tableExists(tableName));
    if ( !metricName.empty() ) REQUIRE(metricName == vectorIndexMetric);
    return tableName;
}
#endif

void QueryTranslatorTest::CHECK_equal(string_view result, string_view expected) {
    if ( result != expected ) {
        stringstream out;
        out << "***Result:   " << result << endl;
        out << "***Expected: " << expected << endl;
        size_t len = std::min(result.size(), expected.size());
        size_t pos;
        for ( pos = 0; pos < len; ++pos ) {
            if ( result[pos] != expected[pos] ) break;
        }
        if ( pos > 100 || result.size() > 100 || expected.size() > 100 ) {
            auto adjPos = std::min(size_t(20), pos);
            out << "\nDifferences begin at " << pos << ":" << endl;
            out << "          ..." << result.substr(pos - adjPos, std::min(result.size() - (pos - adjPos), size_t(100)))
                << endl;
            out << "          ..."
                << expected.substr(pos - adjPos, std::min(expected.size() - (pos - adjPos), size_t(100))) << endl;
            pos = adjPos;
        }
        out << string(13 + pos, ' ') << "^--difference" << endl;
        INFO(out.str());
        FAIL("Incorrect result");
    }
}

#pragma mark - THE TESTS:

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator basic", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['=', ['.', 'name'], 'Puddin\\' Tane']"), "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK_equal(parseWhere("['=', ['.name'], 'Puddin\\' Tane']"), "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK_equal(parseWhere("['AND', ['=', ['.', 'again'], true], ['=', ['.', 'name'], 'Puddin\\' Tane']]"),
                "fl_value(body, 'again') = fl_bool(1) AND fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK_equal(parseWhere("['=', ['+', 2, 2], 5]"), "2 + 2 = 5");
    CHECK_equal(parseWhere("['=', ['power()', 25, ['/', 1, 2]], 5]"), "power(25, 1 / 2) = 5");
    CHECK_equal(parseWhere("['=', ['POWER()', 25, ['/', 1, 2]], 5]"), "power(25, 1 / 2) = 5");
    CHECK_equal(parseWhere("['NOT', ['<', 2, 1]]"), "NOT (2 < 1)");
    CHECK_equal(parseWhere("['-', ['+', 2, 1]]"), "-(2 + 1)");
    CHECK_equal(parseWhere("['*', ['+', 1, 2], ['+', 3, ['-', 4]]]"), "(1 + 2) * (3 + -4)");
    CHECK_equal(parseWhere("['*', ['+', 1, 2], ['-', ['+', 3, 4]]]"), "(1 + 2) * -(3 + 4)");
    CHECK_equal(parseWhere("['BETWEEN', 10, 0, 100]"), "10 BETWEEN 0 AND 100");

    CHECK_equal(parseWhere("['=', ['.', 'candies'], ['[]', 'm&ms', 'jujubes']]"),
                "fl_value(body, 'candies') = array_of('m&ms', 'jujubes')");
    CHECK_equal(parseWhere("['=', ['.address'], {street:'123 Main St', city: ['.city']}]"),
                "fl_value(body, 'address') = dict_of('city', fl_value(body, 'city'), 'street', '123 Main St')");
    CHECK_equal(parseWhere("['=', ['.address'], {}]"), "fl_value(body, 'address') = dict_of()");
    CHECK_equal(parseWhere("['IN', ['.', 'name'], ['[]', 'Webbis', 'Wowbagger']]"),
                "fl_value(body, 'name') IN ('Webbis', 'Wowbagger')");
    CHECK_equal(parseWhere("['NOT IN', ['.', 'name'], ['[]', 'Webbis', 'Wowbagger']]"),
                "fl_value(body, 'name') NOT IN ('Webbis', 'Wowbagger')");
    CHECK_equal(parseWhere("['IN', 'licorice', ['.', 'candies']]"),
                "array_contains(fl_value(body, 'candies'), 'licorice')");
    CHECK_equal(parseWhere("['NOT IN', 7, ['.', 'ages']]"), "NOT array_contains(fl_value(body, 'ages'), 7)");
    CHECK_equal(parseWhere("['.', 'addresses', [1], 'zip']"), "fl_value(body, 'addresses[1].zip')");
    CHECK_equal(parseWhere("['.', 'addresses', [1], 'zip']"), "fl_value(body, 'addresses[1].zip')");

    CHECK_equal(parseWhere("['_.', ['.address'], 'zip']"), "fl_nested_value(fl_value(body, 'address'), 'zip')");
    CHECK_equal(parseWhere("['_.zip', ['.address']]"), "fl_nested_value(fl_value(body, 'address'), 'zip')");
    CHECK_equal(parseWhere("['_.', ['.addresses'], '[0]']"), "fl_nested_value(fl_value(body, 'addresses'), '[0]')");
    CHECK_equal(parseWhere("['_.[0]', ['.addresses']]"), "fl_nested_value(fl_value(body, 'addresses'), '[0]')");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator bindings", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['=', ['$', 'X'], ['$', 7]]"), "$_X = $_7");
    CHECK_equal(parseWhere("['=', ['$X'], ['$', 7]]"), "$_X = $_7");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator special properties", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['ifnull()', ['.', '_id'], ['.', '_sequence']]"), "N1QL_ifnull(key, sequence)");
    CHECK_equal(parseWhere("['ifnull()', ['._id'], ['.', '_sequence']]"), "N1QL_ifnull(key, sequence)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator property contexts", "[Query][QueryTranslator]") {
    // Special cases where a property access uses a different function than fl_value()
    CHECK_equal(parseWhere("['EXISTS', 17]"), "EXISTS 17");
    CHECK_equal(parseWhere("['EXISTS', ['.', 'addresses']]"), "fl_exists(body, 'addresses')");
    CHECK_equal(parseWhere("['EXISTS', ['.addresses']]"), "fl_exists(body, 'addresses')");
    CHECK_equal(parseWhere("['array_count()', ['$', 'X']]"), "array_count($_X)");
    CHECK_equal(parseWhere("['array_count()', ['.', 'addresses']]"), "fl_count(body, 'addresses')");
    CHECK_equal(parseWhere("['array_count()', ['.addresses']]"), "fl_count(body, 'addresses')");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Only Deleted Docs", "[Query][QueryTranslator]") {
    CHECK_equal(parse("['SELECT', {WHAT: ['._id'], WHERE: ['._deleted']}]"),
                "SELECT _doc.key FROM all_default AS _doc WHERE (_doc.flags & 1 != 0)");
    CHECK_equal(parse("['SELECT', {WHAT: ['._id'], WHERE: ['AND',  ['.foo'], ['._deleted']]}]"),
                "SELECT _doc.key FROM all_default AS _doc WHERE fl_value(_doc.body, 'foo') AND (_doc.flags & 1 "
                "!= 0)");
    CHECK_equal(parse("['SELECT', {WHAT: ['._id'], WHERE: ['_.', ['META()'], 'deleted']}]"),
                "SELECT _doc.key FROM all_default AS _doc WHERE (_doc.flags & 1 != 0)");
    CHECK_equal(parse("{WHAT: [['._id']], WHERE: ['._deleted'], FROM: [{AS: 'testdb'}]}"),
                "SELECT testdb.key FROM all_default AS testdb WHERE (testdb.flags & 1 != 0)");
    CHECK_equal(parse("{WHAT: [['._id']], WHERE: ['._deleted'], FROM: [{AS: 'testdb'}]}"),
                "SELECT testdb.key FROM all_default AS testdb WHERE (testdb.flags & 1 != 0)");
    CHECK_equal(parse("{WHAT: [['._id']], WHERE: ['.testdb._deleted'], FROM: [{AS: 'testdb'}]}"),
                "SELECT testdb.key FROM all_default AS testdb WHERE (testdb.flags & 1 != 0)");
    CHECK_equal(parse("{WHAT: ['._id'], WHERE: ['_.', ['META()'], 'deleted'], FROM: [{AS: 'testdb'}]}"),
                "SELECT testdb.key FROM all_default AS testdb WHERE (testdb.flags & 1 != 0)");
    CHECK_equal(parse("{WHAT: ['._id'], WHERE: ['_.', ['META()', 'testdb'], 'deleted'], FROM: [{AS: 'testdb'}]}"),
                "SELECT testdb.key FROM all_default AS testdb WHERE (testdb.flags & 1 != 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Deleted And Live Docs", "[Query][QueryTranslator]") {
    CHECK_equal(parse("['SELECT', {WHAT: ['._id'], WHERE: ['OR',  ['.foo'], ['._deleted']]}]"),
                "SELECT _doc.key FROM all_default AS _doc WHERE fl_value(_doc.body, 'foo') OR (_doc.flags & 1 "
                "!= 0)");
    CHECK_equal(parse("['SELECT', {WHAT: [['META()']]}]"),
                "SELECT fl_result(dict_of('id', _doc.key, 'sequence', _doc.sequence, 'deleted', (_doc.flags & 1 != 0), "
                "'expiration', _doc.expiration, 'revisionID', fl_version(_doc.version))) FROM all_default AS _doc");
    CHECK_equal(parse("['SELECT', {WHAT: [['_.', ['META()'], 'deleted']]}]"),
                "SELECT fl_boolean_result((_doc.flags & 1 != 0)) FROM all_default AS _doc");

    CHECK_equal(parse("['SELECT', {FROM: [{AS: 'base_db'}], WHAT: [['._id'],['._deleted']],"
                      "WHERE: ['AND', ['=', ['._id'], 'doc1'], ['=', ['._deleted'], false]]}]"),
                "SELECT base_db.key, fl_boolean_result((base_db.flags & 1 != 0)) FROM all_default AS"
                " base_db WHERE base_db.key = 'doc1' AND (base_db.flags & 1 != 0) = fl_bool(0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Meta Without Deletion", "[Query][QueryTranslator]") {
    CHECK_equal(parse("['SELECT', {WHAT: [['_.', ['META()'], 'sequence']], WHERE: ['_.', ['META()'], 'sequence']}]"),
                "SELECT _doc.sequence FROM kv_default AS _doc WHERE _doc.sequence AND (_doc.flags & 1 = 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Expiration", "[Query][QueryTranslator]") {
    CHECK_equal(parse("['SELECT', {WHAT: ['._id'], WHERE: ['IS NOT', ['._expiration'], ['MISSING']]}]"),
                "SELECT _doc.key FROM kv_default AS _doc WHERE _doc.expiration IS NOT NULL AND (_doc.flags & "
                "1 = 0)");
    CHECK_equal(parse("['SELECT', {WHAT: ['._expiration'], WHERE: ['IS NOT', ['._expiration'], ['MISSING']]}]"),
                "SELECT _doc.expiration FROM kv_default AS _doc WHERE _doc.expiration IS NOT NULL AND "
                "(_doc.flags & 1 = 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator RevisionID", "[Query][QueryTranslator]") {
    CHECK_equal(parse("['SELECT', {WHAT: ['._id', '._revisionID']}]"),
                "SELECT _doc.key, fl_version(_doc.version) FROM kv_default AS _doc WHERE "
                "(_doc.flags & 1 = 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator ANY", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]"),
                "fl_contains(body, 'names', 'Smith')");
    CHECK_equal(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?X'], 'Smith']]"),
                "fl_contains(body, 'names', 'Smith')");
    CHECK_equal(parseWhere("['ANY', 'X', ['.', 'names'], ['>', ['?', 'X'], 3.125]]"),
                "EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE _X.value > 3.125)");
    CHECK_equal(parseWhere("['EVERY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]"),
                "NOT EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE NOT (_X.value = 'Smith'))");
    CHECK_equal(parseWhere("['ANY AND EVERY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]"),
                "(fl_count(body, 'names') > 0 AND NOT EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE NOT "
                "(_X.value = 'Smith')))");

    CHECK_equal(parse("['SELECT', {FROM: [{AS: 'person'}],\
                                 WHERE: ['ANY', 'X', ['.', 'person', 'names'], ['=', ['?', 'X'], 'Smith']]}]"),
                "SELECT person.key, person.sequence FROM kv_default AS person WHERE fl_contains(person.body, 'names', "
                "'Smith') AND (person.flags & 1 = 0)");
    CHECK_equal(parse("['SELECT', {FROM: [{AS: 'person'}, {AS: 'book', 'ON': 1}],\
                                 WHERE: ['ANY', 'X', ['.', 'book', 'keywords'], ['=', ['?', 'X'], 'horror']]}]"),
                "SELECT person.key, person.sequence FROM kv_default AS person INNER JOIN kv_default AS book ON 1 AND "
                "(book.flags & 1 = 0) WHERE fl_contains(book.body, 'keywords', 'horror') AND (person.flags & 1 = 0)");

    // Non-property calls:
    CHECK_equal(parseWhere("['ANY', 'X', ['pi()'], ['=', ['?X'], 'Smith']]"), "fl_contains(pi(), NULL, 'Smith')");
    CHECK_equal(parseWhere("['EVERY', 'X', ['pi()'], ['=', ['?', 'X'], 'Smith']]"),
                "NOT EXISTS (SELECT 1 FROM fl_each(pi()) AS _X WHERE NOT (_X.value = 'Smith'))");
    CHECK_equal(parse("['SELECT', {FROM: [{AS: 'person'}],\
                     WHERE: ['ANY', 'X', ['pi()'], ['=', ['?', 'X'], 'Smith']]}]"),
                "SELECT person.key, person.sequence FROM kv_default AS person WHERE fl_contains(pi(), NULL, 'Smith') "
                "AND (person.flags & 1 = 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator ANY complex", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?', 'X', 'last'], 'Smith']]"),
                "EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE fl_nested_value(_X.body, 'last') = 'Smith')");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator SELECT", "[Query][QueryTranslator]") {
    CHECK_equal(parse("['SELECT', {WHAT: ['._id'],\
                                 WHERE: ['=', ['.', 'last'], 'Smith'],\
                              ORDER_BY: [['.', 'first'], ['.', 'age']]}]"),
                "SELECT _doc.key FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = 'Smith' AND "
                "(_doc.flags & 1 = 0) ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age')");
    CHECK_equal(parseWhere("['array_count()', ['SELECT',\
                                  {WHAT: ['._id'],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               ORDER_BY: [['.', 'first'], ['.', 'age']]}]]"),
                "array_count(SELECT _doc.key FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = "
                "'Smith' AND (_doc.flags & 1 = 0) ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
    // note this query is lowercase, to test case-insensitivity
    CHECK_equal(parseWhere("['exists', ['select',\
                                  {what: ['._id'],\
                                  where: ['=', ['.', 'last'], 'Smith'],\
                               order_by: [['.', 'first'], ['.', 'age']]}]]"),
                "EXISTS (SELECT _doc.key FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = 'Smith' "
                "AND (_doc.flags & 1 = 0) ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
    CHECK_equal(parseWhere("['EXISTS', ['SELECT',\
                                  {WHAT: [['MAX()', ['.weight']]],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               DISTINCT: true,\
                               GROUP_BY: [['.', 'first'], ['.', 'age']]}]]"),
                "EXISTS (SELECT DISTINCT fl_result(max(fl_value(_doc.body, 'weight'))) FROM kv_default AS _doc WHERE "
                "fl_value(_doc.body, 'last') = 'Smith' AND (_doc.flags & 1 = 0) GROUP BY fl_value(_doc.body, 'first'), "
                "fl_value(_doc.body, 'age'))");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator SELECT WHAT", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['SELECT', {WHAT: ['._id'], WHERE: ['=', ['.', 'last'], 'Smith']}]"),
                "SELECT _doc.key FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = 'Smith' AND "
                "(_doc.flags & 1 = 0)");
    CHECK_equal(parseWhere("['SELECT', {WHAT: [['.first']],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]"),
                "SELECT fl_result(fl_value(_doc.body, 'first')) FROM kv_default AS _doc WHERE fl_value(_doc.body, "
                "'last') = 'Smith' AND (_doc.flags & 1 = 0)");
    CHECK_equal(parseWhere("['SELECT', {WHAT: [['.first'], ['length()', ['.middle']]],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]"),
                "SELECT fl_result(fl_value(_doc.body, 'first')), N1QL_length(fl_value(_doc.body, 'middle')) "
                "FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = 'Smith' AND (_doc.flags & 1 = 0)");
    CHECK_equal(parseWhere("['SELECT', {WHAT: [['.first'], ['AS', ['length()', ['.middle']], 'mid']],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]"),
                "SELECT fl_result(fl_value(_doc.body, 'first')), N1QL_length(fl_value(_doc.body, 'middle')) AS "
                "mid FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = 'Smith' AND (_doc.flags & 1 = 0)");
    // Check the "." operator (like SQL "*"):
    CHECK_equal(parseWhere("['SELECT', {WHAT: ['.'], WHERE: ['=', ['.', 'last'], 'Smith']}]"),
                "SELECT fl_result(fl_root(_doc.body)) FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = "
                "'Smith' AND (_doc.flags & 1 = 0)");
    CHECK_equal(parseWhere("['SELECT', {WHAT: [['.']], WHERE: ['=', ['.', 'last'], 'Smith']}]"),
                "SELECT fl_result(fl_root(_doc.body)) FROM kv_default AS _doc WHERE fl_value(_doc.body, 'last') = "
                "'Smith' AND (_doc.flags & 1 = 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator WHAT aliases", "[Query][QueryTranslator]") {
    CHECK_equal(parse("{WHAT: ['._id', ['AS', ['.dict.key2'], 'answer']], WHERE: ['=', ['.answer'], 1]}"),
                "SELECT _doc.key, fl_result(fl_value(_doc.body, 'dict.key2')) AS answer FROM kv_default AS _doc WHERE "
                "answer = 1 AND (_doc.flags & 1 = 0)");
    // This one was parsed from N1QL query: SELECT `foo.bar`.type FROM _ AS `foo.bar`
    CHECK_equal(
            parse(R"({"FROM":[{"AS":"foo\\.bar","COLLECTION":"_"}],"WHAT":[[".foo\\.bar.type"]]})"),
            R"(SELECT fl_result(fl_value("foo.bar".body, 'type')) FROM kv_default AS "foo.bar" WHERE ("foo.bar".flags & 1 = 0))");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator CASE", "[Query][QueryTranslator]") {
    const char* target = "CASE fl_value(body, 'color') WHEN 'red' THEN 1 WHEN 'green' THEN 2 ELSE fl_null() END";
    CHECK_equal(parseWhere("['CASE', ['.color'], 'red', 1, 'green', 2      ]"), target);
    CHECK_equal(parseWhere("['CASE', ['.color'], 'red', 1, 'green', 2, null]"), target);

    CHECK_equal(parseWhere("['CASE', ['.color'], 'red', 1, 'green', 2, 0]"),
                "CASE fl_value(body, 'color') WHEN 'red' THEN 1 WHEN 'green' THEN 2 ELSE 0 END");

    target = "CASE WHEN 2 = 3 THEN 'wtf' WHEN 2 = 2 THEN 'right' ELSE fl_null() END";
    CHECK_equal(parseWhere("['CASE', null, ['=', 2, 3], 'wtf', ['=', 2, 2], 'right'      ]"), target);
    CHECK_equal(parseWhere("['CASE', null, ['=', 2, 3], 'wtf', ['=', 2, 2], 'right', null]"), target);

    CHECK_equal(parseWhere("['CASE', null, ['=', 2, 3], 'wtf', ['=', 2, 2], 'right', 'whatever']"),
                "CASE WHEN 2 = 3 THEN 'wtf' WHEN 2 = 2 THEN 'right' ELSE 'whatever' END");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator LIKE", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['LIKE', ['.color'], 'b%']"), "fl_value(body, 'color') LIKE 'b%' ESCAPE '\\'");
    CHECK_equal(parseWhere("['LIKE', ['.color'], ['$pattern']]"), "fl_value(body, 'color') LIKE $_pattern ESCAPE '\\'");
    CHECK_equal(parseWhere("['LIKE', ['.color'], ['.pattern']]"),
                "fl_value(body, 'color') LIKE fl_value(body, 'pattern') ESCAPE '\\'");
    // Explicit binary collation:
    CHECK_equal(parseWhere("['COLLATE', {case: true, unicode: false}, ['LIKE', ['.color'], 'b%']]"),
                "fl_value(body, 'color') COLLATE BINARY LIKE 'b%' ESCAPE '\\'");
    // Use fl_like when the collation is non-binary:
    CHECK_equal(parseWhere("['COLLATE', {case: false}, ['LIKE', ['.color'], 'b%']]"),
                "fl_like(fl_value(body, 'color'), 'b%', 'NOCASE')");
    CHECK_equal(parseWhere("['COLLATE', {unicode: true}, ['LIKE', ['.color'], 'b%']]"),
                "fl_like(fl_value(body, 'color'), 'b%', 'LCUnicode____')");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Join", "[Query][QueryTranslator]") {
    CHECK_equal(parse("{WHAT: ['.book.title', '.library.name', '.library'], \
                  FROM: [{as: 'book'}, \
                         {as: 'library', 'on': ['=', ['.book.library'], ['.library._id']]}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(book.body, 'title')), fl_result(fl_value(library.body, 'name')), "
                "fl_result(fl_root(library.body)) FROM kv_default AS book INNER JOIN kv_default AS library ON "
                "fl_value(book.body, 'library') = library.key AND (library.flags & 1 = 0) WHERE fl_value(book.body, "
                "'author') = $_AUTHOR AND (book.flags & 1 = 0)");
    CHECK(usedTableNames == set<string>{"kv_default"});

    // Multiple JOINs (#363):
    CHECK_equal(
            parse("{'WHAT':[['.','session','appId'],['.','user','username'],['.','session','emoId']],\
                  'FROM': [{'as':'session'},\
                           {'as':'user','on':['=',['.','session','emoId'],['.','user','emoId']]},\
                           {'as':'licence','on':['=',['.','session','licenceID'],['.','licence','id']]}],\
                 'WHERE':['AND',['AND',['=',['.','session','type'],'session'],['=',['.','user','type'],'user']],['=',['.','licence','type'],'licence']]}"),
            "SELECT fl_result(fl_value(session.body, 'appId')), fl_result(fl_value(user.body, 'username')), "
            "fl_result(fl_value(session.body, 'emoId')) FROM kv_default AS session INNER JOIN kv_default AS user ON "
            "fl_value(session.body, 'emoId') = fl_value(user.body, 'emoId') AND (user.flags & 1 = 0) INNER JOIN "
            "kv_default AS licence ON fl_value(session.body, 'licenceID') = fl_value(licence.body, 'id') AND "
            "(licence.flags & 1 = 0) WHERE ((fl_value(session.body, 'type') = 'session' AND fl_value(user.body, "
            "'type') = 'user') AND fl_value(licence.body, 'type') = 'licence') AND (session.flags & 1 = 0)");

    CHECK_equal(parse("{WHAT: [['.main.number1'], ['.secondary.number2']],"
                      " FROM: [{AS: 'main'}, {AS: 'secondary', JOIN: 'CROSS'}]}"),
                "SELECT fl_result(fl_value(main.body, 'number1')), fl_result(fl_value(secondary.body, 'number2')) FROM "
                "kv_default AS "
                "main CROSS JOIN kv_default AS secondary ON (secondary.flags & 1 = 0) WHERE (main.flags & 1 = 0)");

    // Result alias and property name are used in different scopes.
    CHECK_equal(parse("{'FROM':[{'AS':'coll','COLLECTION':'_'}],'WHAT':[['AS',['.x'],'label'],['.coll.label']]}"),
                "SELECT fl_result(fl_value(coll.body, 'x')) AS label, fl_result(fl_value(coll.body, 'label')) "
                "FROM kv_default AS coll WHERE (coll.flags & 1 = 0)");
    // CBL-3040:
    CHECK_equal(
            parse(R"r({"WHERE":["AND",["=",[".machines.Type"],"machine"],["OR",["=",[".machines.Disabled"],false],[".machines.Disabled"]]],)r"
                  R"r("WHAT":[[".machines.Id"],["AS",[".machines.Label"],"Label2"],[".machines.ModelId"],["AS",[".models.Label2"],"ModelLabel"]],)r"
                  R"r("FROM":[{"AS":"machines"},{"AS":"models","ON":["=",[".models.Id"],[".machines.ModelId"]],"JOIN":"LEFT OUTER"}]})r"),
            "SELECT fl_result(fl_value(machines.body, 'Id')), fl_result(fl_value(machines.body, 'Label')) AS Label2, "
            "fl_result(fl_value(machines.body, 'ModelId')), fl_result(fl_value(models.body, 'Label2')) AS ModelLabel "
            "FROM kv_default AS machines "
            "LEFT OUTER JOIN kv_default AS models ON fl_value(models.body, 'Id') = fl_value(machines.body, "
            "'ModelId') AND (models.flags & 1 = 0) "
            "WHERE (fl_value(machines.body, 'Type') = 'machine' AND (fl_value(machines.body, 'Disabled') = fl_bool(0) "
            "OR fl_value(machines.body, 'Disabled'))) AND (machines.flags & 1 = 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator SELECT UNNEST", "[Query][QueryTranslator][Unnest]") {
    CHECK_equal(
            parseWhere("['SELECT', {\
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['=', ['.notes'], 'torn']}]"),
            "SELECT book.key, book.sequence FROM kv_default AS book JOIN fl_each(book.body, 'notes') AS notes WHERE "
            "notes.value = 'torn' AND (book.flags & 1 = 0)");
    CHECK_equal(parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]"),
                "SELECT fl_result(notes.value) FROM kv_default AS book JOIN fl_each(book.body, 'notes') AS notes WHERE "
                "fl_nested_value(notes.body, 'page') > 100 AND (book.flags & 1 = 0)");
    //    CHECK_equal(parseWhere("['SELECT', {\
//                      WHAT: ['.notes'], \
//                      FROM: [{as: 'book'}, \
//                             {as: 'notes', 'unnest': ['pi()']}],\
//                     WHERE: ['>', ['.notes.page'], 100]}]"),
    //                "SELECT fl_result(notes.value) FROM kv_default AS book JOIN fl_each(pi()) AS notes WHERE "
    //                "fl_nested_value(notes.body, 'page') > 100 AND (book.flags & 1 = 0)");

    // Unnest of literal array is not allowed for now.
    ExpectException(error::LiteCore, error::InvalidQuery,
                    "the use of a general expression as the object of UNNEST is not supported; "
                    "only a property path is allowed.",
                    [&] {
                        parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['pi()']}],\
                      WHERE: ['>', ['.notes.page'], 100]}]");
                    });
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator SELECT UNNEST optimized", "[Query][QueryTranslator][Unnest]") {
    string hashedUnnestTable = hexName("kv_default:unnest:notes");
    tableNames.insert(hashedUnnestTable);
    if ( '0' <= hashedUnnestTable[0] && hashedUnnestTable[0] <= '9' )
        hashedUnnestTable = "\""s + hashedUnnestTable + "\"";

    CHECK_equal(
            parseWhere("['SELECT', {\
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['=', ['.notes'], 'torn']}]"),
            "SELECT book.key, book.sequence FROM kv_default AS book JOIN " + hashedUnnestTable
                    + " AS notes ON "
                      "notes.docid=book.rowid WHERE fl_unnested_value(notes.body) = 'torn' AND (book.flags & 1 = 0)");
    CHECK_equal(parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]"),
                "SELECT fl_result(fl_unnested_value(notes.body)) FROM kv_default AS book JOIN " + hashedUnnestTable
                        + " AS notes ON notes.docid=book.rowid WHERE fl_unnested_value(notes.body, 'page') > 100 AND "
                          "(book.flags & "
                          "1 = 0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator SELECT UNNEST with collections",
                 "[Query][QueryTranslator][Unnest]") {
    string str = "['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'library'}, \
                             {collection: 'books', as: 'book', 'on': ['=', ['.book.library'], ['.library._id']]}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]";
    // Non-default collection gets unnested:
    tableNames.insert("kv_.books");
    CHECK_equal(parseWhere(str),
                "SELECT fl_result(notes.value) FROM kv_default AS library INNER JOIN \"kv_.books\" AS book ON "
                "fl_value(book.body, 'library') = library.key JOIN fl_each(book.body, 'notes') AS notes WHERE "
                "fl_nested_value(notes.body, 'page') > 100 AND (library.flags & 1 = 0)");

    // Same, but optimized:
    string hashedUnnestTable = hexName("kv_.books:unnest:notes");
    tableNames.insert(hashedUnnestTable);
    if ( '0' <= hashedUnnestTable[0] && hashedUnnestTable[0] <= '9' )
        hashedUnnestTable = "\""s + hashedUnnestTable + "\"";

    CHECK_equal(
            parseWhere(str),
            "SELECT fl_result(fl_unnested_value(notes.body)) FROM kv_default AS library INNER JOIN \"kv_.books\" AS "
            "book ON fl_value(book.body, 'library') = library.key JOIN "
                    + hashedUnnestTable
                    + " AS notes ON "
                      "notes.docid=book.rowid WHERE fl_unnested_value(notes.body, 'page') > 100 AND (library.flags & 1 "
                      "= "
                      "0)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Collate", "[Query][QueryTranslator][Collation]") {
    CHECK_equal(
            parseWhere(
                    "['AND',['COLLATE',{'UNICODE':true,'CASE':false,'DIAC':false},['=',['.Artist'],['$ARTIST']]],['IS'"
                    ",['.Compilation'],['MISSING']]]"),
            "fl_value(body, 'Artist') COLLATE LCUnicode_CD_ = $_ARTIST AND fl_value(body, 'Compilation') IS NULL");
    CHECK_equal(parseWhere("['COLLATE', {unicode: true, locale:'se', case:false}, \
                                  ['=', ['.', 'name'], 'Puddin\\' Tane']]"),
                "fl_value(body, 'name') COLLATE LCUnicode_C__se = 'Puddin'' Tane'");
    CHECK_equal(parseWhere("['COLLATE', {unicode: true, locale:'yue_Hans_CN', case:false}, \
                     ['=', ['.', 'name'], 'Puddin\\' Tane']]"),
                "fl_value(body, 'name') COLLATE LCUnicode_C__yue_Hans_CN = 'Puddin'' Tane'");
    CHECK_equal(parse("{WHAT: ['.book.title'], \
                  FROM: [{as: 'book'}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']], \
              ORDER_BY: [ ['COLLATE', {'unicode':true, 'case':false}, ['.book.title']] ]}"),
                "SELECT fl_result(fl_value(book.body, 'title')) "
                "FROM kv_default AS book "
                "WHERE fl_value(book.body, 'author') = $_AUTHOR AND (book.flags & 1 = 0) "
                "ORDER BY fl_value(book.body, 'title') COLLATE LCUnicode_C__");
    CHECK_equal(parseWhere("['COLLATE',{'CASE':false,'DIAC':true,'LOCALE':'se','UNICODE':false}"
                           ",['=',['.name'],'fred']]"),
                "fl_value(body, 'name') COLLATE NOCASE = 'fred'");
    CHECK_equal(parseWhere("['COLLATE',{'CASE':false,'DIAC':true,'LOCALE':'se','UNICODE':true}"
                           ",['=',['.name'],'fred']]"),
                "fl_value(body, 'name') COLLATE LCUnicode_C__se = 'fred'");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator errors", "[Query][QueryTranslator][!throws]") {
    mustFail("['poop()', 1]");
    mustFail("['power()', 1]");
    mustFail("['power()', 1, 2, 3]");
    mustFail("['CASE', ['.color'], 'red']");
    mustFail("['CASE', null, 'red']");
    mustFail("['_.id']");  // CBL-530
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator weird property names", "[Query][QueryTranslator]") {
    CHECK_equal(parseWhere("['=', ['.', '$foo'], 17]"), "fl_value(body, '\\$foo') = 17");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator FROM collection", "[Query][QueryTranslator]") {
    // Query a nonexistent collection:
    ExpectException(error::LiteCore, error::InvalidQuery, [&] {
        parse("{WHAT: ['.books.title'], \
                   FROM: [{collection: 'books'}],\
                  WHERE: ['=', ['.books.author'], ['$AUTHOR']]}");
    });

    tableNames.insert("kv_.books");

    // Query a non-default collection:
    CHECK_equal(parse("{WHAT: ['.books.title'], \
                  FROM: [{collection: 'books'}],\
                 WHERE: ['=', ['.books.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(books.body, 'title')) "
                "FROM \"kv_.books\" AS books "
                "WHERE fl_value(books.body, 'author') = $_AUTHOR");
    CHECK(usedTableNames == set<string>{"kv_.books"});

    // Add an "AS" alias for the collection:
    CHECK_equal(parse("{WHAT: ['.book.title'], \
                  FROM: [{collection: 'books', as: 'book'}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(book.body, 'title')) "
                "FROM \"kv_.books\" AS book "
                "WHERE fl_value(book.body, 'author') = $_AUTHOR");
    CHECK(usedTableNames == set<string>{"kv_.books"});

    // Join with itself:
    CHECK_equal(parse("{WHAT: ['.book.title', '.library.name', '.library'], \
                  FROM: [{collection: 'books', as: 'book'}, \
                         {as: 'library', 'on': ['=', ['.book.library'], ['.library._id']]}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(book.body, 'title')), fl_result(fl_value(library.body, 'name')), "
                "fl_result(fl_root(library.body)) FROM \"kv_.books\" AS book INNER JOIN \"kv_.books\" AS library ON "
                "fl_value(book.body, 'library') = library.key WHERE fl_value(book.body, 'author') = $_AUTHOR");
    CHECK(usedTableNames == set<string>{"kv_.books"});

    // Join with the default collection:
    CHECK_equal(parse("{WHAT: ['.book.title', '.library.name', '.library'], \
                  FROM: [{collection: 'books', as: 'book'}, \
                         {collection: '_default', as: 'library', 'on': ['=', ['.book.library'], ['.library._id']]}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(book.body, 'title')), fl_result(fl_value(library.body, 'name')), "
                "fl_result(fl_root(library.body)) FROM \"kv_.books\" AS book INNER JOIN kv_default AS library ON "
                "fl_value(book.body, 'library') = library.key AND (library.flags & 1 = 0) WHERE fl_value(book.body, "
                "'author') = $_AUTHOR");
    CHECK(usedTableNames == set<string>{"kv_default", "kv_.books"});

    // Join with a non-default collection:
    tableNames.insert("kv_.library");
    CHECK_equal(parse("{WHAT: ['.book.title', '.library.name', '.library'], \
                  FROM: [{collection: 'books', as: 'book'}, \
                         {collection: 'library', 'on': ['=', ['.book.library'], ['.library._id']]}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(book.body, 'title')), fl_result(fl_value(library.body, 'name')), "
                "fl_result(fl_root(library.body)) FROM \"kv_.books\" AS book INNER JOIN \"kv_.library\" AS library ON "
                "fl_value(book.body, 'library') = library.key WHERE fl_value(book.body, 'author') = $_AUTHOR");
    CHECK(usedTableNames == set<string>{"kv_.books", "kv_.library"});

    // Default collection with non-default join:
    CHECK_equal(parse("{WHAT: ['.book.title', '.library.name', '.library'], \
                  FROM: [{as: 'book'}, \
                         {collection: 'library', 'on': ['=', ['.book.library'], ['.library._id']]}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(book.body, 'title')), fl_result(fl_value(library.body, 'name')), "
                "fl_result(fl_root(library.body)) FROM kv_default AS book INNER JOIN \"kv_.library\" AS library ON "
                "fl_value(book.body, 'library') = library.key WHERE fl_value(book.body, 'author') = $_AUTHOR AND "
                "(book.flags & 1 = 0)");
    CHECK(usedTableNames == set<string>{"kv_default", "kv_.library"});
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator FROM scope", "[Query][QueryTranslator]") {
    tableNames.insert("kv_.banned.books");
    tableNames.insert("kv_.store.customers");
    tableNames.insert("kv_.store2.customers");

    // Query a nonexistent scope:
    ExpectException(error::LiteCore, error::InvalidQuery, [&] {
        parse("{WHAT: ['.books.title'], \
                   FROM: [{scope: 'bestselling', collection: 'books'}],\
                  WHERE: ['=', ['.books.author'], ['$AUTHOR']]}");
    });
    // Query scope w/o collection:
    ExpectException(error::LiteCore, error::InvalidQuery, [&] {
        parse("{WHAT: ['.books.title'], \
                   FROM: [{scope: 'banned'}],\
                  WHERE: ['=', ['.books.author'], ['$AUTHOR']]}");
    });

    // Query a collection in a scope:
    CHECK_equal(parse("{WHAT: ['.books.title'], \
                  FROM: [{scope: 'banned', collection: 'books'}],\
                 WHERE: ['=', ['.banned.books.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(\"banned.books\".body, 'title')) "
                "FROM \"kv_.banned.books\" AS \"banned.books\" "
                "WHERE fl_value(\"banned.books\".body, 'author') = $_AUTHOR");
    CHECK(usedTableNames == set<string>{"kv_.banned.books"});

    // Put the scope name in the collection string:
    CHECK_equal(parse("{WHAT: ['.books.title'], \
                  FROM: [{collection: 'banned.books'}],\
                 WHERE: ['=', ['.books.author'], ['$AUTHOR']]}"),
                "SELECT fl_result(fl_value(\"banned.books\".body, 'title')) "
                "FROM \"kv_.banned.books\" AS \"banned.books\" "
                "WHERE fl_value(\"banned.books\".body, 'author') = $_AUTHOR");
    CHECK(usedTableNames == set<string>{"kv_.banned.books"});

    CHECK_equal(parse("{'FROM':[{'COLLECTION':'customers','SCOPE':'store'}],'WHAT':[['.name']]}"),
                R"(SELECT fl_result(fl_value("store.customers".body, 'name')) FROM )"
                R"("kv_.store.customers" AS "store.customers")");
    CHECK(usedTableNames == set<string>{"kv_.store.customers"});

    CHECK_equal(parse("{'FROM':[{'COLLECTION':'customers','SCOPE':'store'}],"
                      "'WHAT':[['.customers.name']]}"),
                R"(SELECT fl_result(fl_value("store.customers".body, 'name')) )"
                R"(FROM "kv_.store.customers" AS "store.customers")");
    CHECK(usedTableNames == set<string>{"kv_.store.customers"});

    CHECK_equal(parse("{'FROM':[{'COLLECTION':'customers','SCOPE':'store'}],"
                      "'WHAT':[['.store.customers.name']]}"),
                R"(SELECT fl_result(fl_value("store.customers".body, 'name')) )"
                R"(FROM "kv_.store.customers" AS "store.customers")");
    CHECK(usedTableNames == set<string>{"kv_.store.customers"});

    CHECK_equal(parse("{'FROM':[{'COLLECTION':'customers','SCOPE':'store'},"
                      "{'COLLECTION':'customers','JOIN':'INNER','ON':['=',['.store.customers.name'],"
                      "['.store2.customers.name']],'SCOPE':'store2'}],"
                      "'WHAT':[['.store.customers.name'],['.store2.customers.name']]}"),
                R"(SELECT fl_result(fl_value("store.customers".body, 'name')), )"
                R"(fl_result(fl_value("store2.customers".body, 'name')) )"
                R"(FROM "kv_.store.customers" AS "store.customers" INNER JOIN "kv_.store2.customers" )"
                R"(AS "store2.customers" ON fl_value("store.customers".body, 'name') )"
                R"(= fl_value("store2.customers".body, 'name'))");
    CHECK(usedTableNames == set<string>{"kv_.store.customers", "kv_.store2.customers"});
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator nested SELECT", "[Query][QueryTranslator]") {
    //    CHECK_equal(parse("['SELECT',{'WHAT':[['IS',6,9]]}]"),
    //                "SELECT fl_boolean_result(6 IS 9) FROM kv_default AS _doc WHERE (_doc.flags & 1 = 0)");
    CHECK_equal(parse("{'WHAT':[['EXISTS',['SELECT',{'WHAT':[['IS',6,9]]}]]]}"),
                "SELECT fl_boolean_result(EXISTS (SELECT fl_boolean_result(6 IS 9) FROM kv_default AS _doc WHERE "
                "(_doc.flags & 1 = 0))) FROM kv_default AS _doc WHERE (_doc.flags & 1 = 0)");
}

#pragma mark - FTS:

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator SELECT FTS", "[Query][QueryTranslator][FTS]") {
    tableNames.insert("kv_default::bio");
    CHECK_equal(parse("{WHAT: [ ['rank()', 'bio'] ],\
                        WHERE: ['MATCH()', 'bio', 'mobile']}"),
                "SELECT _doc.rowid, offsets(\"<idx1>\".\"kv_default::bio\"), "
                "rank(matchinfo(\"<idx1>\".\"kv_default::bio\")) FROM kv_default AS _doc INNER JOIN "
                "\"kv_default::bio\" AS \"<idx1>\" ON \"<idx1>\".docid = _doc.rowid WHERE "
                "\"<idx1>\".\"kv_default::bio\" MATCH 'mobile' "
                "AND (_doc.flags & 1 = 0)");

    // Non-default collection:
    tableNames.insert("kv_.employees");
    tableNames.insert("kv_.employees::bio");
    CHECK_equal(parse("{FROM: [{collection: 'employees'}],\
                       WHERE: ['MATCH()', 'employees.bio', 'mobile']}"),
                "SELECT employees.rowid, offsets(\"<idx1>\".\"kv_.employees::bio\"), employees.key, employees.sequence "
                "FROM "
                "\"kv_.employees\" AS employees INNER JOIN \"kv_.employees::bio\" AS \"<idx1>\" ON \"<idx1>\".docid = "
                "employees.rowid WHERE "
                "\"<idx1>\".\"kv_.employees::bio\" MATCH 'mobile'");
    // Index name, "bio", does not have to be qualified by the collection if there is only collection in the query
    CHECK_equal(parse("{FROM: [{collection: 'employees'}],\
                        WHERE: ['MATCH()', 'bio', 'mobile']}"),
                "SELECT employees.rowid, offsets(\"<idx1>\".\"kv_.employees::bio\"), employees.key, employees.sequence "
                "FROM "
                "\"kv_.employees\" AS employees INNER JOIN \"kv_.employees::bio\" AS \"<idx1>\" ON \"<idx1>\".docid = "
                "employees.rowid WHERE "
                "\"<idx1>\".\"kv_.employees::bio\" MATCH 'mobile'");

    tableNames.insert("kv_.departments");
    tableNames.insert("kv_.departments::cate");
    CHECK_equal(parse("{\
                FROM: [{collection: 'employees'},\
                       {collection: 'departments', ON: ['=', ['.employees.dept'], ['.departments.name']]}],\
                WHERE: ['AND', ['MATCH()', 'employees.bio', 'mobile'], \
                               ['MATCH()', 'departments.cate', 'engineering']]}"),
                "SELECT employees.rowid, offsets(\"<idx1>\".\"kv_.employees::bio\"), "
                "offsets(\"<idx2>\".\"kv_.departments::cate\"), employees.key, employees.sequence "
                "FROM \"kv_.employees\" AS employees INNER JOIN \"kv_.departments\" AS departments "
                "ON fl_value(employees.body, 'dept') = fl_value(departments.body, 'name') "
                "INNER JOIN \"kv_.employees::bio\" AS \"<idx1>\" ON \"<idx1>\".docid = employees.rowid "
                "INNER JOIN \"kv_.departments::cate\" AS \"<idx2>\" ON \"<idx2>\".docid = departments.rowid "
                "WHERE \"<idx1>\".\"kv_.employees::bio\" MATCH 'mobile' "
                "AND \"<idx2>\".\"kv_.departments::cate\" MATCH 'engineering'");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Buried FTS", "[Query][QueryTranslator][FTS]") {
    tableNames.insert("kv_default::by\\Street");
    parse("['SELECT', {WHERE: ['AND', ['MATCH()', 'byStreet', 'Hwy'],\
                                      ['=', ['.', 'contact', 'address', 'state'], 'CA']]}]");
    ExpectException(error::LiteCore, error::InvalidQuery, "MATCH can only appear at top-level, or in a top-level AND",
                    [this] {
                        parse("['SELECT', {WHERE: ['OR', ['MATCH()', 'byStreet', 'Hwy'],\
                                         ['=', ['.', 'contact', 'address', 'state'], 'CA']]}]");
                    });
}

#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(QueryTranslatorTest, "Predictive Index ID", "[Query][QueryTranslator][Predict]") {
    // It's important that the mapping from PREDICT expressions to table names doesn't change,
    // or it will make existing indexes in existing databases useless.
    QueryTranslator t(*this, "_default", "kv_default");
    auto            doc = Doc::fromJSON(R"-(["PREDICTION()", "8ball", {"number": [".num"]}])-");
    CHECK(t.predictiveTableName(doc.asArray()) == R"(kv_default:predict:0\M\W\K\Sbbzr0gn4\V\V\Vu\Ks\N\E9s\Z\E8o=)");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Vector Search", "[Query][QueryTranslator][VectorSearch]") {
    tableNames.insert("kv_default:vector:vecIndex");
    vectorIndexedProperties.insert({{"kv_default", R"([".vector"])"}, "kv_default:vector:vecIndex"});
    // Pure vector search (no other WHERE criteria):
    CHECK_equal(parse("['SELECT', {"
                      "ORDER_BY: [ ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34]] ],"
                      "LIMIT: 5}]"),
                "SELECT _doc.key, _doc.sequence FROM kv_default AS _doc INNER JOIN (SELECT docid, distance FROM "
                "\"kv_default:vector:vecIndex\" WHERE vector MATCH encode_vector(array_of(12, 34)) LIMIT 5) AS "
                "\"<idx1>\" ON "
                "\"<idx1>\".docid = _doc.rowid WHERE (_doc.flags & 1 = 0) ORDER BY \"<idx1>\".distance LIMIT 5");
    // Pure vector search, specifying metric and numProbes:
    vectorIndexMetric = "cosine";
    CHECK_equal(
            parse("['SELECT', {ORDER_BY: [ ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34], 'cosine', 50] ],"
                  "LIMIT: 5}]"),
            "SELECT _doc.key, _doc.sequence FROM kv_default AS _doc INNER JOIN (SELECT docid, distance FROM "
            "\"kv_default:vector:vecIndex\" WHERE vector MATCH encode_vector(array_of(12, 34)) AND "
            "vectorsearch_probes(vector, 50) LIMIT 5) AS \"<idx1>\" ON "
            "\"<idx1>\".docid = _doc.rowid WHERE (_doc.flags & 1 = 0) ORDER BY \"<idx1>\".distance LIMIT 5");
    // Pure vector search, testing distance in the WHERE:
    vectorIndexMetric = "euclidean2";
    CHECK_equal(parse("['SELECT', {"
                      "WHERE: ['<', ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34]], 1234],"
                      "ORDER_BY: [ ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34]] ],"
                      "LIMIT: 5}]"),
                "SELECT _doc.key, _doc.sequence FROM kv_default AS _doc INNER JOIN (SELECT docid, distance FROM "
                "\"kv_default:vector:vecIndex\" WHERE vector MATCH encode_vector(array_of(12, 34)) LIMIT 5) AS "
                "\"<idx1>\" ON "
                "\"<idx1>\".docid = _doc.rowid WHERE \"<idx1>\".distance < 1234 AND (_doc.flags & 1 = 0) ORDER BY "
                "\"<idx1>\".distance LIMIT 5");
    // Hybrid search:
    CHECK_equal(parse("['SELECT', {WHAT: [ ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34]] ],"
                      "WHERE: ['>', ['._id'], 'x'],"
                      "ORDER_BY: [ ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34]] ]}]"),
                "SELECT \"<idx1>\".distance FROM kv_default AS _doc INNER JOIN \"kv_default:vector:vecIndex\" AS "
                "\"<idx1>\" ON "
                "\"<idx1>\".docid = _doc.rowid AND \"<idx1>\".vector MATCH encode_vector(array_of(12, 34)) WHERE "
                "_doc.key > "
                "'x' AND (_doc.flags & 1 = 0) ORDER BY \"<idx1>\".distance");

    // The optional 'accurate' parameter is ignored, but if given must be false:
    vectorIndexMetric = "cosine";
    CHECK_equal(parse("['SELECT', {"
                      "ORDER_BY: [ ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34], 'cosine', 50, false] ],"
                      "LIMIT: 5}]"),
                "SELECT _doc.key, _doc.sequence FROM kv_default AS _doc INNER JOIN (SELECT docid, distance FROM "
                "\"kv_default:vector:vecIndex\" WHERE vector MATCH encode_vector(array_of(12, 34)) AND "
                "vectorsearch_probes(vector, 50) LIMIT 5) AS \"<idx1>\" ON "
                "\"<idx1>\".docid = _doc.rowid WHERE (_doc.flags & 1 = 0) ORDER BY \"<idx1>\".distance LIMIT 5");
    ExpectException(
            error::LiteCore, error::InvalidQuery, "APPROX_VECTOR_DISTANCE does not support 'accurate'=true", [this] {
                parse("['SELECT', {"
                      "ORDER_BY: [ ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34], 'cosine', 50, true] ],"
                      "LIMIT: 5}]");
            });
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Vector Search Non-Default Collection",
                 "[Query][QueryTranslator][VectorSearch]") {
    tableNames.insert("kv_.coll");
    tableNames.insert("kv_.coll:vector:vecIndex");
    vectorIndexedProperties.insert({{"kv_.coll", R"([".vector"])"}, "kv_.coll:vector:vecIndex"});
    CHECK(parse("['SELECT', {"
                "FROM: [{'COLLECTION':'coll'}],"
                "ORDER_BY: [ ['APPROX_VECTOR_DISTANCE()', ['.coll.vector'], ['[]', 12, 34]] ],"
                "LIMIT: 5}]")
          == "SELECT coll.key, coll.sequence FROM \"kv_.coll\" AS coll INNER JOIN (SELECT docid, distance FROM "
             "\"kv_.coll:vector:vecIndex\" WHERE vector MATCH encode_vector(array_of(12, 34)) LIMIT 5) AS \"<idx1>\" "
             "ON "
             "\"<idx1>\".docid = coll.rowid ORDER BY \"<idx1>\".distance LIMIT 5");
}

TEST_CASE_METHOD(QueryTranslatorTest, "QueryTranslator Buried Vector Search",
                 "[Query][QueryTranslator][VectorSearch]") {
    // Like FTS, vector_match can only be used at top level or within an AND.
    tableNames.insert("kv_default:vector:vecIndex");
    vectorIndexedProperties.insert({{"kv_default", R"([".vector"])"}, "kv_default:vector:vecIndex"});
    parse("['SELECT', {WHERE: ['AND', ['<', ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34]], 1234],\
                                      ['=', ['.', 'contact', 'address', 'state'], 'CA']]}]");
    ExpectException(
            error::LiteCore, error::InvalidQuery, "APPROX_VECTOR_DISTANCE can't be used within an OR in a WHERE clause",
            [this] {
                parse("['SELECT', {WHERE: ['OR', ['<', ['APPROX_VECTOR_DISTANCE()', ['.vector'], ['[]', 12, 34]], 1234],\
                                      ['=', ['.', 'contact', 'address', 'state'], 'CA']]}]");
            });
}
#endif
