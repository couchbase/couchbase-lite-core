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

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;


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

                fleece::Encoder enc;
                enc.beginDictionary();
                enc.writeKey("sentence");
                enc.writeString(kStrings[i]);
                enc.endDictionary();
                alloc_slice body = enc.extractOutput();

                store->set(slice(docID), body, t);
            }
            t.commit();
        }
    }

    void createIndex(KeyStore::IndexOptions options) {
        store->createIndex("sentence"_sl, "[[\".sentence\"]]"_sl, KeyStore::kFullTextIndex, &options);
    }

    void testQuery(const char *queryStr,
                   vector<int> expectedOrder,
                   vector<int> expectedTerms) {
        Retained<Query> query{ store->compileQuery(json5(queryStr)) };
        REQUIRE(query != nullptr);
        unsigned row = 0;
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
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

