//
//  QueryTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 5/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DataFile.hh"
//#include "RecordEnumerator.hh"
#include "Query.hh"
#include "Error.hh"
//#include "FilePath.hh"
#include "Fleece.hh"
#include "Benchmark.hh"

#include "LiteCoreTest.hh"

using namespace litecore;
using namespace std;


static sequence_t writeNumberedDoc(KeyStore *store, int i, slice str, Transaction &t,
                                   DocumentFlags flags =DocumentFlags::kNone) {
    string docID = stringWithFormat("rec-%03d", i);

    fleece::Encoder enc;
    enc.beginDictionary();
    enc.writeKey("num");
    enc.writeInt(i);
    if (str) {
        enc.writeKey("str");
        enc.writeString(str);
    }
    enc.endDictionary();
    alloc_slice body = enc.extractOutput();

    return store->set(slice(docID), nullslice, body, flags, t);
}


// Write 100 docs with Fleece bodies of the form {"num":n} where n is the rec #
static void addNumberedDocs(KeyStore *store) {
    Transaction t(store->dataFile());
    for (int i = 1; i <= 100; i++)
        REQUIRE(writeNumberedDoc(store, i, nullslice, t) == (sequence_t)i);
    t.commit();
}

static vector<string> extractIndexes(slice encodedIndexes) {
    vector<string> retVal;
    const Array *val = Value::fromTrustedData(encodedIndexes)->asArray();
    CHECK(val != nullptr);
    Array::iterator iter(val);
    int size = iter.count();
    for(int i = 0; i < size; i++, ++iter) {
        retVal.emplace_back(iter.value()->asString().asString());
    }
    
    return retVal;
}

TEST_CASE_METHOD(DataFileTestFixture, "Create/Delete Index", "[Query]") {
    KeyStore::IndexOptions options { "en", true };
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter, [=] {
        store->createIndex(""_sl, "[[\".num\"]]"_sl);
    });
    
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter, [=] {
        store->createIndex("\"num\""_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options);
    });
    
    store->createIndex("num"_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options);
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter, [=] {
        store->createIndex("num_second"_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options);
    });
    auto indexes = extractIndexes(store->getIndexes());
    CHECK(indexes.size() == 1);
    CHECK(indexes[0] == "num");
    
    store->deleteIndex("num"_sl);
    store->createIndex("num_second"_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options);
    store->createIndex("num_second"_sl, "[[\".num_second\"]]"_sl, KeyStore::kFullTextIndex, &options);
    indexes = extractIndexes(store->getIndexes());
    CHECK(indexes.size() == 1);
    CHECK(indexes[0] == "num_second");
    
    store->createIndex("num"_sl, "[\".num\"]"_sl);
    store->createIndex("num_second"_sl, "[\".num\"]"_sl);
    indexes = extractIndexes(store->getIndexes());
    CHECK(indexes.size() == 2);
    CHECK(find(indexes.begin(), indexes.end(), "num") != indexes.end());
    CHECK(find(indexes.begin(), indexes.end(), "num_second") != indexes.end());
    store->deleteIndex("num"_sl);
    indexes = extractIndexes(store->getIndexes());
    CHECK(indexes.size() == 1);
    CHECK(indexes[0] == "num_second");
    
    store->deleteIndex("num_second"_sl);
    store->deleteIndex("num_second"_sl); // Duplicate should be no-op
    store->deleteIndex("num_second"_sl);
    store->deleteIndex("num_second"_sl); // Duplicate should be no-op
    indexes = extractIndexes(store->getIndexes());
    CHECK(indexes.size() == 0);
}


TEST_CASE_METHOD(DataFileTestFixture, "Query SELECT", "[Query]") {
    addNumberedDocs(store);
    // Use a (SQL) query based on the Fleece "num" property:
    Retained<Query> query{ store->compileQuery(json5("['AND', ['>=', ['.', 'num'], 30], ['<=', ['.', 'num'], 40]]")) };
    CHECK(query->columnCount() == 2);   // docID and sequence, by default

    for (int pass = 0; pass < 2; ++pass) {
        Stopwatch st;
        int i = 30;
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
        while (e->next()) {
            auto cols = e->columns();
            REQUIRE(e->columns().count() == 2);
            slice docID = cols[0]->asString();
            sequence_t seq = cols[1]->asInt();
            string expectedDocID = stringWithFormat("rec-%03d", i);
            REQUIRE(docID == slice(expectedDocID));
            REQUIRE(seq == (sequence_t)i);
            ++i;
        }
        st.printReport("Query of $.num", i, "row");
        REQUIRE(i == 41);

        // Add an index after the first pass:
        if (pass == 0) {
            Stopwatch st2;
            store->createIndex("num"_sl, "[\".num\"]"_sl);
            st2.printReport("Index on .num", 1, "index");
        }
    }

    // Redundant createIndex should not fail:
    store->createIndex("num"_sl, "[\".num\"]"_sl);
}


TEST_CASE_METHOD(DataFileTestFixture, "Query SELECT WHAT", "[Query]") {
    addNumberedDocs(store);
    Retained<Query> query{ store->compileQuery(json5(
        "{WHAT: ['.num', ['*', ['.num'], ['.num']]], WHERE: ['>', ['.num'], 10]}")) };
    CHECK(query->columnCount() == 2);
    int num = 11;
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    while (e->next()) {
        string expectedDocID = stringWithFormat("rec-%03d", num);
        auto cols = e->columns();
        REQUIRE(cols.count() == 2);
        REQUIRE(cols[0]->asInt() == num);
        REQUIRE(cols[1]->asInt() == num * num);
        ++num;
    }
    REQUIRE(num == 101);
}

TEST_CASE_METHOD(DataFileTestFixture, "Query SELECT All", "[Query]") {
    addNumberedDocs(store);
    Retained<Query> query1{ store->compileQuery(json5("{WHAT: [['.main'], ['*', ['.main.num'], ['.main.num']]], WHERE: ['>', ['.main.num'], 10], FROM: [{AS: 'main'}]}")) };
    Retained<Query> query2{ store->compileQuery(json5("{WHAT: [ '.main',  ['*', ['.main.num'], ['.main.num']]], WHERE: ['>', ['.main.num'], 10], FROM: [{AS: 'main'}]}")) };

    SECTION("Just regular docs") {
    }
    SECTION("Ignore deleted docs") {
        Transaction t(store->dataFile());
        for (int i = 201; i <= 300; i++)
            writeNumberedDoc(store, i, nullslice, t,
                             DocumentFlags::kDeleted | DocumentFlags::kHasAttachments);
        t.commit();
    }

    int num = 11;
    unique_ptr<QueryEnumerator> e(query1->createEnumerator());
    unique_ptr<QueryEnumerator> e2(query1->createEnumerator());
    while (e->next() && e2->next()) {
        string expectedDocID = stringWithFormat("rec-%03d", num);
        auto cols = e->columns();
        auto cols2 = e2->columns();
        REQUIRE(cols.count() == 2);
        REQUIRE(cols2.count() == 2);
        auto star = cols[0]->asDict();
        auto star2 = cols2[0]->asDict();
        REQUIRE(star);
        REQUIRE(star2);
        REQUIRE(star->get("num"_sl)->asInt() == num);
        REQUIRE(star2->get("num"_sl)->asInt() == num);
        REQUIRE(cols[1]->asInt() == num * num);
        REQUIRE(cols2[1]->asInt() == num * num);
        ++num;
    }
    REQUIRE(num == 101);
}


TEST_CASE_METHOD(DataFileTestFixture, "Query null value", "[Query]") {
    {
        Transaction t(store->dataFile());
        fleece::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("n");
        enc.writeNull();
        enc.endDictionary();
        alloc_slice body = enc.extractOutput();
        store->set("null-and-void"_sl, body, t);
        t.commit();
    }

    Retained<Query> query{ store->compileQuery(json5("{WHAT: [['.n'], ['.']]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    auto cols = e->columns();
    REQUIRE(cols.count() == 2);
    CHECK(cols[0]->type() == kNull);
    auto col1 = cols[1]->asDict();
    REQUIRE(col1);
    auto n = col1->get("n"_sl);
    REQUIRE(n);
    CHECK(n->type() == kNull);
}


TEST_CASE_METHOD(DataFileTestFixture, "Query FullText", "[Query]") {
    // Add some text to the database:
    static const char* strings[] = {"FTS5 is an SQLite virtual table module that provides full-text search functionality to database applications.",
        "In their most elementary form, full-text search engines allow the user to efficiently search a large collection of documents for the subset that contain one or more instances of a search term.",
        "The search functionality provided to world wide web users by Google is, among other things, a full-text search engine, as it allows users to search for all documents on the web that contain, for example, the term \"fts5\".",
        "To use FTS5, the user creates an FTS5 virtual table with one or more columns.",
        "Looking for things, searching for things, going on adventures..."};
    {
        Transaction t(store->dataFile());
        for (int i = 0; i < sizeof(strings)/sizeof(strings[0]); i++) {
            string docID = stringWithFormat("rec-%03d", i);

            fleece::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("sentence");
            enc.writeString(strings[i]);
            enc.endDictionary();
            alloc_slice body = enc.extractOutput();

            store->set(slice(docID), body, t);
        }
        t.commit();
    }

    KeyStore::IndexOptions options;
    SECTION("English") {
        options = {"en", true};
    }
    SECTION("Unknown language") {
        options = {"elbonian", true};
    }

    store->createIndex("sentence"_sl, "[[\".sentence\"]]"_sl, KeyStore::kFullTextIndex, &options);

    Retained<Query> query{ store->compileQuery(json5(
        "['SELECT', {'WHERE': ['MATCH', ['.', 'sentence'], 'search'],\
                    ORDER_BY: [['DESC', ['rank()', ['.', 'sentence']]]],\
                    WHAT: [['.sentence']]}]")) };
    REQUIRE(query != nullptr);
    unsigned rows = 0;
    int expectedOrder[] = {1, 2, 0, 4};
    int expectedTerms[] = {3, 3, 1, 1};
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    while (e->next()) {
        auto cols = e->columns();
        REQUIRE(cols.count() == 1);
        CHECK(cols[0]->asString() == slice(strings[expectedOrder[rows]]));
        CHECK(e->hasFullText());
        CHECK(e->fullTextTerms().size() == expectedTerms[rows]);
        for (auto term : e->fullTextTerms()) {
            auto word = string(strings[expectedOrder[rows]] + term.start, term.length);
            CHECK(word == (rows == 3 ? "searching" : "search"));
        }
        CHECK((string)query->getMatchedText(e->fullTextID()) == strings[expectedOrder[rows]]);
        ++rows;
    }
    if (strcmp(options.stemmer, "en") == 0) {
        CHECK(rows == 4);
    } else {
        // Non-English stemmer will not find "searching" in the 4th document
        CHECK(rows == 3);
    }

    // Redundant createIndex should not fail:
    store->createIndex("sentence"_sl, "[[\".sentence\"]]"_sl, KeyStore::kFullTextIndex, &options);
}


TEST_CASE_METHOD(DataFileTestFixture, "Query refresh", "[Query]") {
    addNumberedDocs(store);
    Retained<Query> query{ store->compileQuery(json5(
                     "{WHAT: ['.num', ['*', ['.num'], ['.num']]], WHERE: ['>', ['.num'], 10]}")) };
    CHECK(query->columnCount() == 2);
    int num = 11;
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    while (e->next())
        ++num;
    REQUIRE(num == 101);

    CHECK(e->refresh() == nullptr);

    // Add a doc that doesn't alter the query:
    {
        Transaction t(db);
        writeNumberedDoc(store, -1, nullslice, t);
        t.commit();
    }
    CHECK(e->refresh() == nullptr);

#if 0 //FIX: This doesn't work yet, because the doc's sequence and revID are in the query results,
      // and those do change...
    // Modify a doc in a way that doesn't affect the query results
    {
        Transaction t(db);
        writeNumberedDoc(store, 20, "howdy"_sl, t);
        t.commit();
    }
    CHECK(e->refresh() == nullptr);
#endif

    // Delete one of the docs in the query -- this does trigger a refresh:
    {
        Transaction t(db);
        store->set("rec-030"_sl, "2-ffff"_sl, nullslice, DocumentFlags::kDeleted, t);
        t.commit();
    }

    unique_ptr<QueryEnumerator> e2(e->refresh());
    REQUIRE(e2 != nullptr);

    num = 11;
    while (e2->next())
        ++num;
    CHECK(num == 100);
}
