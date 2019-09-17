//
// FTSTest.cc
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

#include "DataFile.hh"
#include "Query.hh"
#include "Error.hh"
#include "StringUtil.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;
using namespace fleece::impl;


class FTSTest : public DataFileTestFixture {
public:
    static constexpr const char* const kStrings[] = {
        "FTS5 is an SQLite virtual table module that provides full-text search functionality to database applications.",
        "In their most elementary form, full-text search engines allow the user to efficiently search a large collection of documents for the subset that contain one or more instances of a search term.",
        "The search functionality provided to world wide web users by Google is, among other things, a full-text search engine, as it allows users to search for all documents on the web that contain, for example, the term \"fts5\".",
        "To use FTS5, the user creates an FTS5 virtual table with one or more columns.",
        "Looking for things, searching for things, going on adventures..."};

    FTSTest() {
        {
            Transaction t(store->dataFile());
            for (int i = 0; i < sizeof(kStrings)/sizeof(kStrings[0]); i++) {
                string docID = stringWithFormat("rec-%03d", i);

                fleece::impl::Encoder enc;
                enc.beginDictionary();
                enc.writeKey("sentence");
                enc.writeString(kStrings[i]);
                enc.endDictionary();
                alloc_slice body = enc.finish();

                store->set(slice(docID), body, t);
            }
            t.commit();
        }
    }

    void createIndex(KeyStore::IndexOptions options) {
        store->createIndex({"sentence", KeyStore::kFullTextIndex, alloc_slice("[[\".sentence\"]]")}, &options);
    }

    void testQuery(const char *queryStr,
                   vector<int> expectedOrder,
                   vector<int> expectedTerms) {
        Retained<Query> query{ store->compileQuery(json5(queryStr)) };
        REQUIRE(query != nullptr);
        unsigned row = 0;
        Retained<QueryEnumerator> e(query->createEnumerator());
        while (e->next()) {
            auto cols = e->columns();
            REQUIRE(cols.count() == 1);
            REQUIRE(row < expectedOrder.size());
            int stringNo = expectedOrder[row];
            slice sentence = cols[0]->asString();
            CHECK(sentence == slice(kStrings[stringNo]));
            CHECK(e->hasFullText());
            CHECK(e->fullTextTerms().size() == expectedTerms[row]);
            for (auto term : e->fullTextTerms()) {
                auto word = string(sentence).substr(term.start, term.length);
                //CHECK(word == (stringNo == 4 ? "searching" : "search"));
                CHECK(query->getMatchedText(term) == sentence);
            }
            ++row;
        }
        CHECK(row == expectedOrder.size());
    }
};

constexpr const char* const FTSTest::kStrings[5];


TEST_CASE_METHOD(FTSTest, "Query Full-Text English", "[Query][FTS]") {
    createIndex({"english", true});
    testQuery(
        "['SELECT', {'WHERE': ['MATCH', 'sentence', 'search'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0, 4},
              {3, 3, 1, 1});
}


TEST_CASE_METHOD(FTSTest, "Query Full-Text English_US", "[Query][FTS]") {
    // Check that language+country code is allowed:
    createIndex({"en_US", true});
    testQuery(
        "['SELECT', {'WHERE': ['MATCH', 'sentence', 'search'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0, 4},
              {3, 3, 1, 1});
}


TEST_CASE_METHOD(FTSTest, "Query Full-Text Unsupported Language", "[Query][FTS]") {
    createIndex({"elbonian", true});
    testQuery(
        "['SELECT', {'WHERE': ['MATCH', 'sentence', 'search'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0},
              {3, 3, 1});
}


TEST_CASE_METHOD(FTSTest, "Query Full-Text Stop-words", "[Query][FTS]") {
    // Check that English stop-words like "the" and "is" are being ignored by FTS.
    createIndex({"en", true});
    testQuery(
        "['SELECT', {'WHERE': ['MATCH', 'sentence', 'the search is'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 2, 0, 4},
              {3, 3, 1, 1});
}


TEST_CASE_METHOD(FTSTest, "Query Full-Text No stop-words", "[Query][FTS]") {
    SECTION("Recreate index, no-op") {
        createIndex({"en", true, false, ""});
    }
    SECTION("Recreate index") {
        createIndex({"en", true});
    }
    createIndex({"en", true, false, ""});
    testQuery(
        "['SELECT', {'WHERE': ['MATCH', 'sentence', 'the search is'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {2},
              {7});
}


TEST_CASE_METHOD(FTSTest, "Query Full-Text Custom stop-words", "[Query][FTS]") {
    createIndex({"en", true, false, "the a an"});
    testQuery(
        "['SELECT', {'WHERE': ['MATCH', 'sentence', 'the search is'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {2, 0},
              {4, 2});
}


TEST_CASE_METHOD(FTSTest, "Query Full-Text Stop-words In Target", "[Query][FTS]") {
    // Stop-words should not be removed from the target string of the MATCH. Otherwise, the
    // MATCH in this test would turn into 'f* and *' (since "on" is a stop-word) which is invalid.
    // https://github.com/couchbase/couchbase-lite-core/issues/626
    createIndex({"en", true});
    testQuery(
        "['SELECT', {'WHERE': ['MATCH', 'sentence', 'f* AND on*'],\
                    ORDER_BY: [['DESC', ['rank()', 'sentence']]],\
                        WHAT: [['.sentence']]}]",
              {1, 3},
              {3, 3});
}

TEST_CASE_METHOD(FTSTest, "Test with array values", "[FTS][Query]") {
    // Tests fix for <https://issues.couchbase.com/browse/CBL-218>

    store->deleteIndex("List"_sl);
    SECTION("Create Index First") {
        KeyStore::IndexOptions options { "en", false, true };
        CHECK(store->createIndex("List"_sl, "[[\".List\"]]"_sl, KeyStore::kFullTextIndex, &options));
    }

    {
        Transaction t(store->dataFile());
        writeDoc(slice("movies"), DocumentFlags::kNone, t, [=](Encoder &enc) {
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

        writeDoc(slice("action"), DocumentFlags::kNone, t, [=](Encoder &enc) {
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

        writeDoc(slice("thriller"), DocumentFlags::kNone, t, [=](Encoder &enc) {
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

        writeDoc(slice("history"), DocumentFlags::kNone, t, [=](Encoder &enc) {
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
        KeyStore::IndexOptions options { "en", false, true };
        CHECK(store->createIndex("List"_sl, "[[\".List\"]]"_sl, KeyStore::kFullTextIndex, &options));
    }

    Retained<Query> query = store->compileQuery(json5("{WHAT: [ '._id'], WHERE: ['MATCH', 'List', ['$title']]}"));
    vector<slice> titles { "the"_sl, "shawshank"_sl, "redemption"_sl, "(1994)"_sl, "godfather"_sl, "(1972)"_sl,
        "part"_sl, "ii"_sl, "(1974)"_sl, "dark"_sl, "knight"_sl, "(2008)"_sl, "12"_sl, "angry"_sl, "men"_sl,
        "(1957)"_sl, "schindler's"_sl, "list"_sl, "(1993)"_sl, "lord"_sl, "of"_sl, "rings"_sl, "return"_sl,
        "king"_sl, "(2003)"_sl, "pulp"_sl, "fiction"_sl, "avengers"_sl, "endgame"_sl, "(2019)"_sl, "good"_sl,
        "bad"_sl, "and"_sl, "ugly"_sl, "(1966)"_sl, "mountain"_sl, "(2016)"_sl, "inception"_sl, "(2010)"_sl,
        "matrix"_sl, "(1999)"_sl, "star"_sl, "wars"_sl, "episode"_sl, "v"_sl, "empire"_sl, "strikes"_sl,
        "(1980)"_sl, "uri"_sl, "surgical"_sl, "strike"_sl, "léon"_sl, "professional"_sl, "iv"_sl, "a"_sl,
        "new"_sl, "hope"_sl, "(1977)"_sl, "dangal"_sl, "ayla"_sl, "daughter"_sl, "war"_sl, "(2017)"_sl,
        "braveheart"_sl, "(1995)"_sl, "amadeus"_sl, "(1984)"_sl, "lawrence"_sl, "arabia"_sl, "(1962)"_sl,
        "downfall"_sl, "(2004)"_sl, "raise"_sl, "red"_sl, "lantern"_sl, "(1991)"_sl, "message"_sl, "(1976)"_sl,
        "andrei"_sl, "rublev"_sl, "great"_sl, "escape"_sl, "(1963)"_sl, "usual"_sl, "suspects"_sl, "se7en"_sl,
        "silence"_sl, "lambs"_sl, "andhadhun"_sl, "(2018)"_sl, "prestige"_sl, "(2006)"_sl, "departed"_sl,
        "memento"_sl, "(2000)"_sl
    };
    vector<int> expected { 0, 1, 1, 3, 1, 1,
        1, 2, 1, 3, 3, 3, 1, 1, 1,
        1, 2, 2, 2, 1, 0, 1, 1,
        1, 1, 1, 1, 2, 2, 2, 1,
        1, 0, 1, 2, 1, 1, 2, 2,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 2, 2, 1, 0,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 2, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 2, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1,
        1, 1
    };

    for(int i = 0; i < titles.size(); i++) {
        Log("Checking '%.*s'...", SPLAT(titles[i]));
        Encoder e;
        e.beginDictionary(1);
        e.writeKey("title");
        e.writeString(titles[i]);
        e.endDictionary();
        Query::Options queryOptions(e.finish());
        Retained<QueryEnumerator> results(query->createEnumerator(&queryOptions));        
        CHECK(results->getRowCount() == expected[i]);
    }
}

TEST_CASE_METHOD(FTSTest, "Test with Dictionary Values", "[FTS][Query]") {
    {
        Transaction t(store->dataFile());
        writeDoc(slice("dict"), DocumentFlags::kNone, t, [=](Encoder &enc) {
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

    KeyStore::IndexOptions options { "en", false, false };
    CHECK(store->createIndex("fts"_sl, "[[\".dict_value\"]]"_sl, KeyStore::kFullTextIndex, &options));
    Retained<Query> query = store->compileQuery(json5("{WHAT: [ '._id'], WHERE: ['MATCH', 'fts', 'bar']}"));
    Retained<QueryEnumerator> results(query->createEnumerator(nullptr));        
    CHECK(results->getRowCount() == 1);
}

TEST_CASE_METHOD(FTSTest, "Test with non-string values", "[FTS][Query]") {
    {
        Transaction t(store->dataFile());
        writeDoc("1"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeBool(true);
        });

        writeDoc("2"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeBool(false);
        });

        writeDoc("3"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeBool(true);
        });

        writeDoc("3"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeInt(-41);
        });

        writeDoc("4"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeUInt(42);
        });

        writeDoc("5"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeFloat(3.14f);
        });

        writeDoc("6"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeDouble(1.234);
        });

        t.commit();
    }

    slice valueToCheck;
    SECTION("Boolean True") {
        valueToCheck = "T"_sl;
    }

    SECTION("Boolean False") {
        valueToCheck = "F"_sl;
    }

    SECTION("Integer") {
        valueToCheck = "-41"_sl;
    }

    SECTION("Unsigned") {
        valueToCheck = "42"_sl;
    }

    SECTION("Float") {
        valueToCheck = "3.14"_sl;
    }

    SECTION("Double") {
        valueToCheck = "1.234"_sl;
    }

    KeyStore::IndexOptions options { "en", false, true };
    CHECK(store->createIndex("fts"_sl, "[[\".value\"]]"_sl, KeyStore::kFullTextIndex, &options));
    Retained<Query> query = store->compileQuery(json5("{WHAT: [ '._id'], WHERE: ['MATCH', 'fts', ['$value']]}"));
    Encoder e;
    e.beginDictionary(1);
    e.writeKey("value");
    e.writeString(valueToCheck);
    e.endDictionary();
    Query::Options queryOptions(e.finish());
    Retained<QueryEnumerator> results(query->createEnumerator(&queryOptions));        
    CHECK(results->getRowCount() == 1);
}
