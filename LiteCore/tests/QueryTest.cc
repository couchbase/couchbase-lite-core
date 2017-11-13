//
//  QueryTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 5/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DataFile.hh"
#include "Query.hh"
#include "Error.hh"
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

TEST_CASE_METHOD(DataFileTestFixture, "Create/Delete Index", "[Query][FTS]") {
    KeyStore::IndexOptions options { "en", true };
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter, [=] {
        store->createIndex(""_sl, "[[\".num\"]]"_sl);
    });
    
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter, [=] {
        store->createIndex("\"num\""_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options);
    });
    
    store->createIndex("num"_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options);
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


TEST_CASE_METHOD(DataFileTestFixture, "Query boolean", "[Query]") {
    {
        Transaction t(store->dataFile());
        for(int i = 0; i < 2; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            
            fleece::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("value");
            enc.writeBool(i == 0);
            enc.endDictionary();
            alloc_slice body = enc.extractOutput();
            
            store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        }
        
        // Integer 0 and 1 would have fooled ISBOOLEAN() before
        for(int i = 2; i < 4; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            
            fleece::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("value");
            enc.writeInt(i - 2);
            enc.endDictionary();
            alloc_slice body = enc.extractOutput();
            
            store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        }
        
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{WHAT: ['._id'], WHERE: ['ISBOOLEAN()', ['.value']]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    int i = 1;
    while (e->next()) {
        CHECK(e->columns()[0]->asString().asString() == stringWithFormat("rec-%03d", i++));
    }
}
    
#pragma mark Targeted N1QL tests
    
TEST_CASE_METHOD(DataFileTestFixture, "Query array length", "[Query]") {
    {
        Transaction t(store->dataFile());
        for(int i = 0; i < 2; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            
            fleece::Encoder enc;
            enc.beginDictionary(1);
            enc.writeKey("value");
            enc.beginArray(i + 1);
            for(int j = 0; j < i + 1; j++) {
                enc.writeInt(j);
            }
            
            enc.endArray();
            enc.endDictionary();
            alloc_slice body = enc.extractOutput();
            
            store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        }
        
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{WHAT: ['._id'], WHERE: ['>', ['ARRAY_LENGTH()', ['.value']], 1]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "rec-002"_sl);
}

TEST_CASE_METHOD(DataFileTestFixture, "Query missing and null", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "doc1";
        
        fleece::Encoder enc;
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeNull();
        enc.writeKey("real_value");
        enc.writeInt(1);
        enc.endDictionary();
        alloc_slice body = enc.extractOutput();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        enc.reset();
        docID = "doc2";
        enc.beginDictionary(2);
        enc.writeKey("value");
        enc.writeNull();
        enc.writeKey("atai");
        enc.writeInt(1);
        enc.endDictionary();
        body = enc.extractOutput();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': ['._id'], WHERE: ['=', ['IFMISSING()', ['.bogus'], ['.value']], null]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);
    
    query =store->compileQuery(json5(
        "{'WHAT': ['._id'], WHERE: ['=', ['IFMISSINGORNULL()', ['.atai'], ['.value']], 1]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);
    
    query =store->compileQuery(json5(
        "{'WHAT': ['._id'], WHERE: ['=', ['IFNULL()', ['.real_value'], ['.atai']], 1]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);
}

TEST_CASE_METHOD(DataFileTestFixture, "Query regex", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "doc1";
        
        fleece::Encoder enc;
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeString("awesome value");
        enc.endDictionary();
        alloc_slice body = enc.extractOutput();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        enc.reset();
        docID = "doc2";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeString("cool value");
        enc.endDictionary();
        body = enc.extractOutput();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        enc.reset();
        docID = "doc3";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeString("invalid");
        enc.endDictionary();
        body = enc.extractOutput();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': ['._id'], WHERE: ['REGEXP_CONTAINS()', ['.value'], '.*? value']}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);
    
    query = store->compileQuery(json5(
        "{'WHAT': ['._id'], WHERE: ['REGEXP_LIKE()', ['.value'], '.*? value']}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);
    
    query = store->compileQuery(json5(
        "{'WHAT': ['._id'], WHERE: ['>', ['REGEXP_POSITION()', ['.value'], '[ ]+value'], 4]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
}
