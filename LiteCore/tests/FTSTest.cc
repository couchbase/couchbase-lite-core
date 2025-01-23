//
// FTSTest.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "DataFile.hh"
#include "Query.hh"
#include "StringUtil.hh"
#include "FleeceImpl.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;
using namespace fleece::impl;

class FTSTest : public DataFileTestFixture {
  public:
    static constexpr const char* const kStrings[] = {
            "FTS5 is an SQLite virtual table module that provides full-text search functionality to database "
            "applications.",
            "In their most elementary form, full-text search engines allow the user to efficiently search a large "
            "collection of documents for the subset that contain one or more instances of a search term.",
            "The search functionality provided to world wide web users by Google is, among other things, a "
            "full-text search engine, as it allows users to search for all documents on the web that contain, for "
            "example, the term \"fts5\".",
            "To use FTS5, the user creates an FTS5 virtual table with one or more columns.",
            "Looking for things, searching for things, going on adventures..."};

    vector<string> _stringsInDB;

    static DataFile::Options* dbOptions() {
        static DataFile::Options sOptions = DataFile::Options::defaults;
        sOptions.keyStores.sequences      = false;  // make it easier to overwrite docs in this test
        return &sOptions;
    }

    FTSTest() : DataFileTestFixture(0, dbOptions()) {
        ExclusiveTransaction t(store->dataFile());
        for ( int i = 0; i < sizeof(kStrings) / sizeof(kStrings[0]); i++ ) createDoc(t, i, kStrings[i]);
        t.commit();
    }

    void createDoc(ExclusiveTransaction& t, int i, const string& sentence) {
        string docID = stringWithFormat("rec-%03d", i);

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("sentence");
        enc.writeString(sentence);
        enc.endDictionary();
        alloc_slice body = enc.finish();

        store->setKV(docID, body, t);

        if ( _stringsInDB.size() <= i ) _stringsInDB.resize(i + 1);
        _stringsInDB[i] = sentence;
    }

    void createIndex(IndexSpec::FTSOptions options) {
        store->createIndex("sentence", "[[\".sentence\"]]", IndexSpec::kFullText, options);
    }

    void testQuery(const char* queryStr, vector<int> expectedOrder, vector<int> expectedTerms,
                   QueryLanguage language = QueryLanguage::kJSON) {
        Retained<Query> query{
                db->compileQuery(language == QueryLanguage::kJSON ? json5(queryStr) : slice(queryStr), language)};
        REQUIRE(query != nullptr);
        unsigned                  row = 0;
        Retained<QueryEnumerator> e(query->createEnumerator());
        while ( e->next() ) {
            auto cols = e->columns();
            REQUIRE(cols.count() == 1);
            REQUIRE(row < expectedOrder.size());
            int   stringNo = expectedOrder[row];
            slice sentence = cols[0]->asString();
            CHECK(sentence == slice(_stringsInDB[stringNo]));
            CHECK(e->hasFullText());
            CHECK(e->fullTextTerms().size() == expectedTerms[row]);
            for ( auto term : e->fullTextTerms() ) {
                auto word = string(sentence).substr(term.start, term.length);
                //CHECK(word == (stringNo == 4 ? "searching" : "search"));
                CHECK(query->getMatchedText(term) == sentence);
            }
            ++row;
        }
        CHECK(row == expectedOrder.size());
    }
};

TEST_CASE_METHOD(FTSTest, "Query Full-Text English", "[Query][FTS]") {
    createIndex({"english", true});
    const char*   queryStr = nullptr;
    QueryLanguage lang     = QueryLanguage::kJSON;
    SECTION("JSON query") {
        queryStr = "['SELECT', {'WHERE': ['MATCH()', 'sentence', 'search'],\
                               ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                                   WHAT: [['.sentence']]}]";
        lang     = QueryLanguage::kJSON;
    }
    SECTION("N1QL query") {
        queryStr = "SELECT sentence FROM _ WHERE MATCH(sentence, 'search') ORDER BY rank(sentence) DESC";
        lang     = QueryLanguage::kN1QL;
    }
    testQuery(queryStr, {1, 2, 0, 4}, {3, 3, 1, 1}, lang);
}

TEST_CASE_METHOD(FTSTest, "Query Full-Text English_US", "[Query][FTS]") {
    // Check that language+country code is allowed:
    createIndex({"en_US", true});
    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'search'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0, 4}, {3, 3, 1, 1});
}

TEST_CASE_METHOD(FTSTest, "Query Full-Text Unsupported Language", "[Query][FTS]") {
    createIndex({"elbonian", true});
    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'search'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0}, {3, 3, 1});
}

TEST_CASE_METHOD(FTSTest, "Query Full-Text Stop-words", "[Query][FTS]") {
    // Check that English stop-words like "the" and "is" are being ignored by FTS.
    createIndex({"en", true});
    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'the search is'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0, 4}, {3, 3, 1, 1});
}

TEST_CASE_METHOD(FTSTest, "Query Full-Text No stop-words", "[Query][FTS]") {
    SECTION("Recreate index, no-op") { createIndex({"en", true, false, ""}); }
    SECTION("Recreate index") { createIndex({"en", true}); }
    createIndex({"en", true, false, ""});
    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'the search is'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {2}, {7});
}

TEST_CASE_METHOD(FTSTest, "Query Full-Text Custom stop-words", "[Query][FTS]") {
    createIndex({"en", true, false, "the a an"});
    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'the search is'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {2, 0}, {4, 2});
}

TEST_CASE_METHOD(FTSTest, "Query Full-Text Stop-words In Target", "[Query][FTS]") {
    // Stop-words should not be removed from the target string of the MATCH. Otherwise, the
    // MATCH in this test would turn into 'f* and *' (since "on" is a stop-word) which is invalid.
    // https://github.com/couchbase/couchbase-lite-core/issues/626
    createIndex({"en", true});
    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'f* AND on*'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 3}, {3, 3});
}

TEST_CASE_METHOD(FTSTest, "Query Full-Text Partial Index", "[Query][FTS]") {
    // the WHERE clause prevents row 4 from being indexed/searched.
    IndexSpec::FTSOptions options{"english", true};

    SECTION("JSON Index Spec with combinged \"what\" and \"where\"") {
        REQUIRE(store->createIndex({"sentence",
                                    IndexSpec::kFullText,
                                    R"-({"WHAT": [[".sentence"]], "WHERE": [">", ["length()", [".sentence"]], 70]})-",
                                    {},
                                    QueryLanguage::kJSON,
                                    options}));
    }

    SECTION("JSON Index Spec with separate \"what\" and \"where\"") {
        REQUIRE(store->createIndex({"sentence", IndexSpec::kFullText, R"-([[".sentence"]])-",
                                    R"-([">", ["length()", [".sentence"]], 70])-", QueryLanguage::kJSON, options}));
    }

    SECTION("N1QL Index Spec") {
        REQUIRE(store->createIndex({"sentence", IndexSpec::kFullText, "sentence", "length(sentence) > 70",
                                    QueryLanguage::kN1QL, options}));
    }

    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'search'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0}, {3, 3, 1});

    // Now update docs so one is removed from the index and another added:
    {
        ExclusiveTransaction t(store->dataFile());
        createDoc(t, 4,
                  "The expression on the right must be a text value specifying the term to search for. For the "
                  "table-valued function syntax, the term to search for is specified as the first table argument.");
        createDoc(t, 1, "Search, search");
        t.commit();
    }

    testQuery("['SELECT', {'WHERE': ['MATCH()', 'sentence', 'search'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {2, 4, 0}, {3, 2, 1});
}

TEST_CASE_METHOD(FTSTest, "Test with array values", "[FTS][Query]") {
    // Tests fix for <https://issues.couchbase.com/browse/CBL-218>

    store->deleteIndex("List"_sl);
    const char* const jsonQuery = "{WHAT: [ '._id'], WHERE: ['MATCH()', 'List', ['$title']]}";
    const char* const n1qlQuery = "SELECT META().id FROM _ WHERE MATCH(List, $title)";
    const char*       queryStr  = nullptr;
    QueryLanguage     lang      = QueryLanguage::kJSON;

    SECTION("Create Index First") {
        IndexSpec::FTSOptions options{"en", false, true};
        CHECK(store->createIndex("List"_sl, "List"_sl, QueryLanguage::kN1QL, IndexSpec::kFullText, options));
        queryStr = jsonQuery;
        lang     = QueryLanguage::kJSON;
    }

    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc(slice("movies"), DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("Title");
            enc.writeString("Top 100 movies");
            enc.writeKey("List");
            enc.beginArray(10);
            enc.writeString("The Shawshank Redemption (1994)");
            enc.writeString("The Godfather (1972)");
            enc.writeString("The Godfather: Part II (1974)");
            enc.writeString("The Dark Knight (2008)");
            enc.writeString("12 Angry Men (1957)");
            enc.writeString("Schindler's List (1993)");
            enc.writeString("The Lord of the Rings: The Return of the King (2003)");
            enc.writeString("Pulp Fiction (1994)");
            enc.writeString("Avengers: Endgame (2019)");
            enc.writeString("The Good, the Bad and the Ugly (1966)");
            enc.endArray();
        });

        writeDoc(slice("action"), DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("Title");
            enc.writeString("Top 100 Action movies");
            enc.writeKey("List");
            enc.beginArray(10);
            enc.writeString("The Mountain II (2016)");
            enc.writeString("Avengers: Endgame (2019)");
            enc.writeString("The Dark Knight (2008)");
            enc.writeString("Inception (2010)");
            enc.writeString("The Matrix (1999)");
            enc.writeString("Star Wars: Episode V - The Empire Strikes Back (1980)");
            enc.writeString("Uri: The Surgical Strike (2019)");
            enc.writeString("Léon: The Professional (1994)");
            enc.writeString("Star Wars: Episode IV - A New Hope (1977)");
            enc.writeString("Dangal (2016)");
            enc.endArray();
        });

        writeDoc(slice("thriller"), DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("Title");
            enc.writeString("Top 100 Thriller movies");
            enc.writeKey("List");
            enc.beginArray(10);
            enc.writeString("The Dark Knight (2008)");
            enc.writeString("Inception (2010)");
            enc.writeString("The Usual Suspects (1995)");
            enc.writeString("Se7en (1995)");
            enc.writeString("Léon: The Professional (1994)");
            enc.writeString("The Silence of the Lambs (1991)");
            enc.writeString("Andhadhun (2018)");
            enc.writeString("The Prestige (2006)");
            enc.writeString("The Departed (2006)");
            enc.writeString("Memento (2000)");
            enc.endArray();
        });

        writeDoc(slice("history"), DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("Title");
            enc.writeString("Top 100 History movies");
            enc.writeKey("List");
            enc.beginArray(10);
            enc.writeString("Schindler's List (1993)");
            enc.writeString("Ayla: The Daughter of War (2017)");
            enc.writeString("Braveheart (1995)");
            enc.writeString("Amadeus (1984)");
            enc.writeString("Lawrence of Arabia (1962)");
            enc.writeString("Downfall (2004)");
            enc.writeString("Raise the Red Lantern (1991)");
            enc.writeString("The Message (1976)");
            enc.writeString("Andrei Rublev (1966)");
            enc.writeString("The Great Escape (1963)");
            enc.endArray();
        });
        t.commit();
    }

    SECTION("Create Index After") {
        IndexSpec::FTSOptions options{"en", false, true};
        CHECK(store->createIndex("List"_sl, "[[\".List\"]]"_sl, IndexSpec::kFullText, options));
        queryStr = n1qlQuery;
        lang     = QueryLanguage::kN1QL;
    }

    Retained<Query> query = db->compileQuery(lang == QueryLanguage::kJSON ? json5(queryStr) : slice(queryStr), lang);
    vector<slice>   titles{
            "the"_sl,      "shawshank"_sl, "redemption"_sl, "(1994)"_sl,     "godfather"_sl,    "(1972)"_sl,
            "part"_sl,     "ii"_sl,        "(1974)"_sl,     "dark"_sl,       "knight"_sl,       "(2008)"_sl,
            "12"_sl,       "angry"_sl,     "men"_sl,        "(1957)"_sl,     "schindler's"_sl,  "list"_sl,
            "(1993)"_sl,   "lord"_sl,      "of"_sl,         "rings"_sl,      "return"_sl,       "king"_sl,
            "(2003)"_sl,   "pulp"_sl,      "fiction"_sl,    "avengers"_sl,   "endgame"_sl,      "(2019)"_sl,
            "good"_sl,     "bad"_sl,       "and"_sl,        "ugly"_sl,       "(1966)"_sl,       "mountain"_sl,
            "(2016)"_sl,   "inception"_sl, "(2010)"_sl,     "matrix"_sl,     "(1999)"_sl,       "star"_sl,
            "wars"_sl,     "episode"_sl,   "v"_sl,          "empire"_sl,     "strikes"_sl,      "(1980)"_sl,
            "uri"_sl,      "surgical"_sl,  "strike"_sl,     "léon"_sl,       "professional"_sl, "iv"_sl,
            "a"_sl,        "new"_sl,       "hope"_sl,       "(1977)"_sl,     "dangal"_sl,       "ayla"_sl,
            "daughter"_sl, "war"_sl,       "(2017)"_sl,     "braveheart"_sl, "(1995)"_sl,       "amadeus"_sl,
            "(1984)"_sl,   "lawrence"_sl,  "arabia"_sl,     "(1962)"_sl,     "downfall"_sl,     "(2004)"_sl,
            "raise"_sl,    "red"_sl,       "lantern"_sl,    "(1991)"_sl,     "message"_sl,      "(1976)"_sl,
            "andrei"_sl,   "rublev"_sl,    "great"_sl,      "escape"_sl,     "(1963)"_sl,       "usual"_sl,
            "suspects"_sl, "se7en"_sl,     "silence"_sl,    "lambs"_sl,      "andhadhun"_sl,    "(2018)"_sl,
            "prestige"_sl, "(2006)"_sl,    "departed"_sl,   "memento"_sl,    "(2000)"_sl};
    vector<int> expected{0, 1, 1, 3, 1, 1, 1, 2, 1, 3, 3, 3, 1, 1, 1, 1, 2, 2, 2, 1, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1,
                         0, 1, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                         2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

    for ( int i = 0; i < titles.size(); i++ ) {
        Log("Checking '%.*s'...", SPLAT(titles[i]));
        Encoder e;
        e.beginDictionary(1);
        e.writeKey("title");
        e.writeString(titles[i]);
        e.endDictionary();
        Query::Options            queryOptions(e.finish());
        Retained<QueryEnumerator> results(query->createEnumerator(&queryOptions));
        CHECK(results->getRowCount() == expected[i]);
    }
}

TEST_CASE_METHOD(FTSTest, "Test with Dictionary Values", "[FTS][Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc(slice("dict"), DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("dict_value");
            enc.beginDictionary(2);
            enc.writeKey("value_one"_sl);
            enc.writeString("foo"_sl);
            enc.writeKey("value_two");
            enc.writeString("bars"_sl);
            enc.endDictionary();
        });

        t.commit();
    }

    IndexSpec::FTSOptions options{"en", false, false};

    SECTION("JSON index expression") {
        CHECK(store->createIndex("fts"_sl, "[[\".dict_value\"]]", IndexSpec::kFullText, options));
    }
    SECTION("N1QL index expression") {
        CHECK(store->createIndex("fts"_sl, "dict_value", QueryLanguage::kN1QL, IndexSpec::kFullText, options));
    }

    Retained<Query>           query = store->compileQuery(json5("{WHAT: [ '._id'], WHERE: ['MATCH()', 'fts', 'bar']}"));
    Retained<QueryEnumerator> results(query->createEnumerator(nullptr));
    CHECK(results->getRowCount() == 1);
}

TEST_CASE_METHOD(FTSTest, "Test with non-string values", "[FTS][Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeBool(true);
        });

        writeDoc("2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeBool(false);
        });

        writeDoc("3"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeBool(true);
        });

        writeDoc("3"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeInt(-41);
        });

        writeDoc("4"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeUInt(42);
        });

        writeDoc("5"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeFloat(3.14f);
        });

        writeDoc("6"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeDouble(1.234);
        });

        t.commit();
    }

    slice valueToCheck;
    SECTION("Boolean True") { valueToCheck = "T"_sl; }

    SECTION("Boolean False") { valueToCheck = "F"_sl; }

    SECTION("Integer") { valueToCheck = "-41"_sl; }

    SECTION("Unsigned") { valueToCheck = "42"_sl; }

    SECTION("Float") { valueToCheck = "3.14"_sl; }

    SECTION("Double") { valueToCheck = "1.234"_sl; }

    IndexSpec::FTSOptions options{"en", false, true};
    CHECK(store->createIndex("fts"_sl, "[[\".value\"]]"_sl, IndexSpec::kFullText, options));
    Retained<Query> query = db->compileQuery(json5("{WHAT: [ '._id'], WHERE: ['MATCH()', 'fts', ['$value']]}"));
    Encoder         e;
    e.beginDictionary(1);
    e.writeKey("value");
    e.writeString(valueToCheck);
    e.endDictionary();
    Query::Options            queryOptions(e.finish());
    Retained<QueryEnumerator> results(query->createEnumerator(&queryOptions));
    CHECK(results->getRowCount() == 1);
}

TEST_CASE_METHOD(FTSTest, "Missing FTS columns", "[FTS][Query]") {
    // CBL-977: FTS rows have special meta columns in front, and
    // so the missing columns need to ignore those

    IndexSpec::FTSOptions options{"", false, false};
    CHECK(store->createIndex("ftsIndex"_sl, "[[\".key-fts\"]]"_sl, IndexSpec::kFullText, options));

    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("key-fts");
            enc.writeString("some terms to search against");
            enc.writeKey("key-2");
            enc.writeString("foo");
            enc.writeKey("key-used-once");
            enc.writeString("bar");
        });

        writeDoc("sample2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("key-fts");
            enc.writeString("other terms to search against");
            enc.writeKey("key-2");
            enc.writeString("bar");
        });

        t.commit();
    }

    string queries[] = {
            json5("{WHAT: [['.key-2'],['.key-used-once'],['.key-unused']], WHERE: ['MATCH()', 'ftsIndex', 'against']}"),
            json5("{WHAT: [['.key-unused'],['.key-used-once'],['.key-2']], WHERE: ['MATCH()', 'ftsIndex', "
                  "'against']}")};

    int expectedMissing = 2;
    for ( const auto& q : queries ) {
        Retained<Query>           query = db->compileQuery(q);
        Retained<QueryEnumerator> results(query->createEnumerator(nullptr));
        CHECK(results->getRowCount() == 2);

        CHECK(results->next());
        CHECK(results->missingColumns() == (1ULL << expectedMissing));

        CHECK(results->next());
        CHECK(results->missingColumns() == ((1ULL << expectedMissing) | 1 << 1));
        expectedMissing = 0;
    }
}

TEST_CASE_METHOD(FTSTest, "No alias on MATCH", "[FTS][Query]") {
    // Test that the first parameter of `MATCH` doesn't need a db alias even when there's an `AS`,
    // as long as there's only one alias.
    createIndex({"english", true});

    const string indexSpecs[] = {"sentence", "testdb.sentence"};
    for ( const string& spec : indexSpecs ) {
        string q = R"-({"WHAT":[["._id"],[".sentence"]],"FROM":[{"AS":"testdb"}],"WHERE":["MATCH()",")-" + spec
                   + R"-(","'Dummie woman'"],"ORDER_BY":[["DESC",["RANK()","sentence"]]]})-";
        Retained<Query> query = db->compileQuery(q);  // just verify it compiles
    }
}
