//
// QueryTest.cc
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

#include "QueryTest.hh"

using namespace fleece::impl;


TEST_CASE_METHOD(QueryTest, "Create/Delete Index", "[Query][FTS]") {
    addArrayDocs();

    KeyStore::IndexOptions options { "en", true };
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter, [=] {
        store->createIndex(""_sl, "[[\".num\"]]"_sl);
    });
    
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter, [=] {
        store->createIndex("\"num\""_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options);
    });

    CHECK(store->createIndex("num"_sl, "[[\".num\"]]"_sl, KeyStore::kValueIndex, &options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num"}));
    CHECK(!store->createIndex("num"_sl, "[[\".num\"]]"_sl, KeyStore::kValueIndex, &options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num"}));

    CHECK(store->createIndex("num"_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options));
    CHECK(!store->createIndex("num"_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num"}));

    store->deleteIndex("num"_sl);
    CHECK(store->createIndex("num_second"_sl, "[[\".num\"]]"_sl, KeyStore::kFullTextIndex, &options));
    CHECK(store->createIndex("num_second"_sl, "[[\".num_second\"]]"_sl, KeyStore::kFullTextIndex, &options));
    CHECK(extractIndexes(store->getIndexes()) == vector<string>{"num_second"});
    
    CHECK(store->createIndex("num"_sl, "[\".num\"]"_sl));
    CHECK(store->createIndex("num_second"_sl, "[\".num\"]"_sl));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num", "num_second"}));
    store->deleteIndex("num"_sl);
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num_second"}));

    CHECK(store->createIndex("array_1st"_sl, "[[\".numbers\"]]"_sl, KeyStore::kArrayIndex, &options));
    CHECK(!store->createIndex("array_1st"_sl, "[[\".numbers\"]]"_sl, KeyStore::kArrayIndex, &options));
    CHECK(store->createIndex("array_2nd"_sl, "[[\".numbers\"],[\".key\"]]"_sl, KeyStore::kArrayIndex, &options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"array_1st", "array_2nd", "num_second"}));

    store->deleteIndex("num_second"_sl);
    store->deleteIndex("num_second"_sl); // Duplicate should be no-op
    store->deleteIndex("array_1st"_sl);
    store->deleteIndex("array_1st"_sl); // Duplicate should be no-op
    store->deleteIndex("array_2nd"_sl);
    store->deleteIndex("array_2nd"_sl); // Duplicate should be no-op
    CHECK(extractIndexes(store->getIndexes()) == vector<string>{ });
}


TEST_CASE_METHOD(QueryTest, "Create/Delete Array Index", "[Query][ArrayIndex]") {
    addArrayDocs();
    store->createIndex("nums"_sl, "[[\".numbers\"]]"_sl, KeyStore::kArrayIndex);
    store->deleteIndex("nums"_sl);
}


TEST_CASE_METHOD(QueryTest, "Query SELECT", "[Query]") {
    addNumberedDocs();
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


TEST_CASE_METHOD(QueryTest, "Query SELECT WHAT", "[Query]") {
    addNumberedDocs();
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


TEST_CASE_METHOD(QueryTest, "Query SELECT All", "[Query]") {
    addNumberedDocs();
    Retained<Query> query1{ store->compileQuery(json5("{WHAT: [['.main'], ['*', ['.main.num'], ['.main.num']]], WHERE: ['>', ['.main.num'], 10], FROM: [{AS: 'main'}]}")) };
    Retained<Query> query2{ store->compileQuery(json5("{WHAT: [ '.main',  ['*', ['.main.num'], ['.main.num']]], WHERE: ['>', ['.main.num'], 10], FROM: [{AS: 'main'}]}")) };

    SECTION("Just regular docs") {
    }
    SECTION("Ignore deleted docs") {
        Transaction t(store->dataFile());
        for (int i = 201; i <= 300; i++)
            writeNumberedDoc(i, nullslice, t,
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


TEST_CASE_METHOD(QueryTest, "Query null value", "[Query]") {
    {
        Transaction t(store->dataFile());
        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("n");
        enc.writeNull();
        enc.endDictionary();
        alloc_slice body = enc.finish();
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


TEST_CASE_METHOD(QueryTest, "Query refresh", "[Query]") {
    addNumberedDocs();
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
        writeNumberedDoc(-1, nullslice, t);
        t.commit();
    }
    CHECK(e->refresh() == nullptr);

#if 0 //FIX: This doesn't work yet, because the doc's sequence and revID are in the query results,
      // and those do change...
    // Modify a doc in a way that doesn't affect the query results
    {
        Transaction t(db);
        writeNumberedDoc(20, "howdy"_sl, t);
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


TEST_CASE_METHOD(QueryTest, "Query boolean", "[Query]") {
    {
        Transaction t(store->dataFile());
        for(int i = 0; i < 2; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            
            fleece::impl::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("value");
            enc.writeBool(i == 0);
            enc.endDictionary();
            alloc_slice body = enc.finish();
            
            store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        }
        
        // Integer 0 and 1 would have fooled ISBOOLEAN() before
        for(int i = 2; i < 4; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            
            fleece::impl::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("value");
            enc.writeInt(i - 2);
            enc.endDictionary();
            alloc_slice body = enc.finish();
            
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


TEST_CASE_METHOD(QueryTest, "Query weird property names", "[Query]") {
    // For <https://github.com/couchbase/couchbase-lite-core/issues/545>
    {
        Transaction t(store->dataFile());
        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("$foo");    enc.writeInt(17);
        enc.writeKey("?foo");    enc.writeInt(18);
        enc.writeKey("[foo");    enc.writeInt(19);
        enc.writeKey(".foo");    enc.writeInt(20);
        enc.writeKey("f$o?o[o."); enc.writeInt(21);
        enc.endDictionary();
        alloc_slice body = enc.finish();
        store->set("doc1"_sl, nullslice, body, DocumentFlags::kNone, t);
        t.commit();
    }

    CHECK(rowsInQuery(json5("{WHAT: ['.$foo']}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['=', ['.', '$foo'], 17]}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['EXISTS', ['.', '$foo']]}")) == 1);

    CHECK(rowsInQuery(json5("{WHAT: ['.\\\\?foo']}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['=', ['.', '?foo'], 18]}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['EXISTS', ['.', '?foo']]}")) == 1);

    CHECK(rowsInQuery(json5("{WHAT: ['.\\\\[foo']}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['=', ['.', '[foo'], 19]}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['EXISTS', ['.', '[foo']]}")) == 1);

    CHECK(rowsInQuery(json5("{WHAT: ['.\\\\.foo']}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['=', ['.', '.foo'], 20]}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['EXISTS', ['.', '.foo']]}")) == 1);

    // Finally the boss battle:
    CHECK(rowsInQuery(json5("{WHAT: ['.f$o\\\\?o\\\\[o\\\\.']}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['=', ['.', 'f$o?o[o.'], 21]}")) == 1);
    CHECK(rowsInQuery(json5("{WHERE: ['EXISTS', ['.', 'f$o?o[o.']]}")) == 1);
}


#pragma mark Targeted N1QL tests
    
TEST_CASE_METHOD(QueryTest, "Query array length", "[Query]") {
    {
        Transaction t(store->dataFile());
        for(int i = 0; i < 2; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            
            fleece::impl::Encoder enc;
            enc.beginDictionary(1);
            enc.writeKey("value");
            enc.beginArray(i + 1);
            for(int j = 0; j < i + 1; j++) {
                enc.writeInt(j);
            }
            
            enc.endArray();
            enc.endDictionary();
            alloc_slice body = enc.finish();
            
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


TEST_CASE_METHOD(QueryTest, "Query missing and null", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "doc1";
        
        fleece::impl::Encoder enc;
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeNull();
        enc.writeKey("real_value");
        enc.writeInt(1);
        enc.endDictionary();
        alloc_slice body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        enc.reset();
        docID = "doc2";
        enc.beginDictionary(2);
        enc.writeKey("value");
        enc.writeNull();
        enc.writeKey("atai");
        enc.writeInt(1);
        enc.endDictionary();
        body = enc.finish();
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
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);

    query =store->compileQuery(json5(
        "{'WHAT': ['._id'], WHERE: ['=', ['IFMISSINGORNULL()', ['.real_value'], ['.atai']], 1]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);
}


TEST_CASE_METHOD(QueryTest, "Query regex", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "doc1";
        
        fleece::impl::Encoder enc;
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeString("awesome value");
        enc.endDictionary();
        alloc_slice body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        enc.reset();
        docID = "doc2";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeString("cool value");
        enc.endDictionary();
        body = enc.finish();
        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        
        enc.reset();
        docID = "doc3";
        enc.beginDictionary(1);
        enc.writeKey("value");
        enc.writeString("invalid");
        enc.endDictionary();
        body = enc.finish();
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
    
    query = store->compileQuery(json5(
       "{'WHAT': [['REGEXP_REPLACE()', ['.value'], '.*?value', 'nothing']]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 3);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "nothing"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "nothing"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "invalid"_sl);
}


TEST_CASE_METHOD(QueryTest, "Query type check", "[Query]") {
    {
        Transaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': [['TYPE()', ['.value']], ['._id']], WHERE: ['ISARRAY()', ['.value']]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "array"_sl);
    CHECK(e->columns()[1]->asString() == "doc1"_sl);
    
    query = store->compileQuery(json5(
        "{'WHAT': [['TYPE()', ['.value']], ['._id'], ['.value']], WHERE: ['ISNUMBER()', ['.value']]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "number"_sl);
    CHECK(e->columns()[1]->asString() == "doc3"_sl);
    CHECK(e->columns()[2]->asDouble() == 4.5);
    
    query = store->compileQuery(json5(
        "{'WHAT': [['TYPE()', ['.value']], ['._id'], ['.value']], WHERE: ['ISSTRING()', ['.value']]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "string"_sl);
    CHECK(e->columns()[1]->asString() == "doc2"_sl);
    CHECK(e->columns()[2]->asString() == "cool value"_sl);
    
    query = store->compileQuery(json5(
        "{'WHAT': [['TYPE()', ['.value']], ['._id']], WHERE: ['ISOBJECT()', ['.value']]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "object"_sl);
    CHECK(e->columns()[1]->asString() == "doc4"_sl);
    
    query = store->compileQuery(json5(
        "{'WHAT': [['TYPE()', ['.value']], ['._id'], ['.value']], WHERE: ['ISBOOLEAN()', ['.value']]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "boolean"_sl);
    CHECK(e->columns()[1]->asString() == "doc5"_sl);
    CHECK(e->columns()[2]->asBool());
    
    query = store->compileQuery(json5(
        "{'WHAT': [['TYPE()', ['.value']], ['._id']], WHERE: ['ISATOM()', ['.value']]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 3);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "string"_sl);
    CHECK(e->columns()[1]->asString() == "doc2"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "number"_sl);
    CHECK(e->columns()[1]->asString() == "doc3"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "boolean"_sl);
    CHECK(e->columns()[1]->asString() == "doc5"_sl);
}


TEST_CASE_METHOD(QueryTest, "Query toboolean", "[Query]") {
    {
        Transaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        writeFalselyDocs(t);
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': [['TOBOOLEAN()', ['.value']]]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 8);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
}


TEST_CASE_METHOD(QueryTest, "Query toatom", "[Query]") {
    {
        Transaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        writeFalselyDocs(t);
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': [['TOATOM()', ['.value']]]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 8);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "cool value"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 4.5);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "FTW"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
}


TEST_CASE_METHOD(QueryTest, "Query tonumber", "[Query]") {
    {
        Transaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
    "{'WHAT': [['TONUMBER()', ['.value']]]}")) };
    unique_ptr<QueryEnumerator> e;
    {
        ExpectingExceptions x;      // tonumber() will internally throw/catch an exception while indexing
        e.reset(query->createEnumerator());
    }
    REQUIRE(e->getRowCount() == 5);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 0.0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 0.0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 4.5);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 0.0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 1.0);
}


TEST_CASE_METHOD(QueryTest, "Query tostring", "[Query]") {
    {
        Transaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        writeFalselyDocs(t);
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': [['TOSTRING()', ['.value']]]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 8);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "cool value"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString().asString().substr(0, 3) == "4.5");
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "true"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "false"_sl);
}


TEST_CASE_METHOD(QueryTest, "Query HAVING", "[Query]") {
    {
        Transaction t(store->dataFile());

        char docID[6];
        for(int i = 0; i < 20; i++) {
            sprintf(docID, "doc%02d", i);
            fleece::impl::Encoder enc;
            enc.beginDictionary(1);
            enc.writeKey("identifier");
            enc.writeInt(i >= 5 ? i >= 15 ? 3 : 2 : 1);
            enc.writeKey("index");
            enc.writeInt(i);
            enc.endDictionary();
            alloc_slice body = enc.finish();
            store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);
        }

        t.commit();
    }
    

    Retained<Query> query{ store->compileQuery(json5(
        "{'WHAT': ['.identifier', ['COUNT()', ['.index']]], 'GROUP_BY': ['.identifier'], 'HAVING': ['=', ['.identifier'], 1]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
    CHECK(e->columns()[1]->asInt() == 5);

    query = store->compileQuery(json5(
        "{'WHAT': ['.identifier', ['COUNT()', ['.index']]], 'GROUP_BY': ['.identifier'], 'HAVING': ['!=', ['.identifier'], 1]}"));
    e.reset(query->createEnumerator());

    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 2);
    CHECK(e->columns()[1]->asInt() == 10);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 3);
    CHECK(e->columns()[1]->asInt() == 5);
}


TEST_CASE_METHOD(QueryTest, "Query Functions", "[Query]") {
    {
        Transaction t(store->dataFile());
        writeNumberedDoc(1, nullslice, t);

        t.commit();
    }

    auto query = store->compileQuery(json5(
        "{'WHAT': [['e()']]}"));
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == M_E);

    query = store->compileQuery(json5(
        "{'WHAT': [['pi()']]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == M_PI);

    vector<string> what { 
        "[['atan2()', ['.num'], ['*', ['.num'], 2]]]",
        "[['acos()', ['.num']]]",
        "[['asin()', ['.num']]]",
        "[['atan()', ['.num']]]",
        "[['cos()', ['.num']]]",
        "[['degrees()', ['.num']]]",
        "[['radians()', ['.num']]]",
        "[['sin()', ['.num']]]",
        "[['tan()', ['.num']]]"
    };

    vector<double> results {
        atan2(2, 1),
        acos(1),
        asin(1),
        atan(1),
        cos(1),
        180.0 / M_PI,
        M_PI / 180.0,
        sin(1),
        tan(1),
    };

    for(int i = 0; i < what.size(); i++) {
        query = store->compileQuery(json5(
        "{'WHAT': " + what[i] + "}"));
        e.reset(query->createEnumerator());
        REQUIRE(e->getRowCount() == 1);
        REQUIRE(e->next());
        CHECK(e->columns()[0]->asDouble() == results[i]);
    }

    query = store->compileQuery(json5(
        "{'WHAT': [['sign()', ['.num']]]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
}


TEST_CASE_METHOD(QueryTest, "Query unsigned", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "rec-001";

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("num");
        enc.writeUInt(1);
        enc.endDictionary();
        alloc_slice body = enc.finish();

        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        t.commit();
    }

    auto query = store->compileQuery(json5(
        "{'WHAT': ['.num']}"));
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asUnsigned() == 1U);
    CHECK(e->columns()[0]->asInt() == 1);

}


// Test for #341, "kData fleece type unable to be queried"
TEST_CASE_METHOD(QueryTest, "Query data type", "[Query]") {
     {
        Transaction t(store->dataFile());
        string docID = "rec-001";

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("num");
        enc.writeData("number one"_sl);
        enc.endDictionary();
        alloc_slice body = enc.finish();

        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        t.commit();
    }

    auto query = store->compileQuery(json5(
        "{'WHAT': ['.num']}"));
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asData() == "number one"_sl);

    query = store->compileQuery(json5(
        "{'WHAT': [['type()', ['.num']]]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "binary"_sl);
}


TEST_CASE_METHOD(QueryTest, "Missing columns", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "rec-001";

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("num");
        enc.writeInt(1234);
        enc.writeKey("string");
        enc.writeString("FOO");
        enc.endDictionary();
        alloc_slice body = enc.finish();

        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        t.commit();
    }

    auto query = store->compileQuery(json5(
        "{'WHAT': ['.num', '.string']}"));
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->missingColumns() == 0);
    CHECK(e->columns()[0]->toJSONString() == "1234");
    CHECK(e->columns()[1]->toJSONString() == "\"FOO\"");

    query = store->compileQuery(json5(
        "{'WHAT': ['.bogus', '.num', '.nope', '.string', '.gone']}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->missingColumns() == 0x15);       // binary 10101, i.e. cols 0, 2, 4 are missing
    CHECK(e->columns()[1]->toJSONString() == "1234");
    CHECK(e->columns()[3]->toJSONString() == "\"FOO\"");
}


TEST_CASE_METHOD(QueryTest, "Negative Limit / Offset", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "rec-001";

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("num");
        enc.writeInt(1234);
        enc.writeKey("string");
        enc.writeString("FOO");
        enc.endDictionary();
        alloc_slice body = enc.finish();

        store->set(slice(docID), nullslice, body, DocumentFlags::kNone, t);

        t.commit();
    }

    auto query = store->compileQuery(json5(
        "{'WHAT': ['.num', '.string'], 'LIMIT': -1}"));
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    CHECK(e->getRowCount() == 0);

    query = store->compileQuery(json5(
        "{'WHAT': ['.num', '.string'], 'LIMIT': 100, 'OFFSET': -1}"));
    e.reset(query->createEnumerator());
    CHECK(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSONString() == "1234");
    CHECK(e->columns()[1]->toJSONString() == "\"FOO\"");

    Query::Options opts;
    opts.paramBindings = R"({"lim": -1})"_sl;
    query = store->compileQuery(json5(
        "{'WHAT': ['.num', '.string'], 'LIMIT': ['$lim']}"));
    e.reset(query->createEnumerator(&opts));
    CHECK(e->getRowCount() == 0);

    opts.paramBindings = R"({"lim": 100, "skip": -1})"_sl;
    query = store->compileQuery(json5(
        "{'WHAT': ['.num', '.string'], 'LIMIT': ['$lim'], 'OFFSET': ['$skip']}"));
    e.reset(query->createEnumerator(&opts));
    CHECK(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSONString() == "1234");
    CHECK(e->columns()[1]->toJSONString() == "\"FOO\"");
}


TEST_CASE_METHOD(QueryTest, "Query JOINs", "[Query]") {
     {
        Transaction t(store->dataFile());
        string docID = "rec-00";

        for(int i = 0; i < 10; i++) {
            stringstream ss(docID);
            ss << i + 1;

            fleece::impl::Encoder enc;
            enc.beginDictionary();
            enc.writeKey("num1");
            enc.writeInt(i);
            enc.writeKey("num2");
            enc.writeInt(10 - i);
            enc.endDictionary();
            alloc_slice body = enc.finish();

            store->set(slice(ss.str()), nullslice, body, DocumentFlags::kNone, t);
        }

        fleece::impl::Encoder enc;
        enc.beginDictionary();
        enc.writeKey("theone");
        enc.writeInt(4);
        enc.endDictionary();
        alloc_slice body = enc.finish();

        store->set("magic"_sl, nullslice, body, DocumentFlags::kNone, t);

        t.commit();
    }

    auto query = store->compileQuery(json5(
        "{'WHAT': [['.main.num1']], 'FROM': [{'AS':'main'}, {'AS':'secondary', 'ON': ['=', ['.main.num1'], ['.secondary.theone']]}]}"));
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 4);

    query = store->compileQuery(json5(
        "{'WHAT': [['.main.num1'], ['.secondary.theone']], 'FROM': [{'AS':'main'}, {'AS':'secondary', 'ON': ['=', ['.main.num1'], ['.secondary.theone']], 'JOIN':'LEFT OUTER'}]}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 11);
    e->seek(4);
    CHECK(e->columns()[0]->asInt() == 4);
    CHECK(e->columns()[1]->asInt() == 4);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 5);
    CHECK(e->columns()[1]->asInt() == 0);
    
    // NEED TO DEFINE THIS BEHAVIOR.  WHAT IS THE CORRECT RESULT?  THE BELOW FAILS!
    /*query = store->compileQuery(json5(
        "{'WHAT': [['.main.num1'], ['.secondary.num2']], 'FROM': [{'AS':'main'}, {'AS':'secondary', 'JOIN':'CROSS'}], 'ORDER_BY': ['.secondary.num2']}"));
    e.reset(query->createEnumerator());
    REQUIRE(e->getRowCount() == 100);
    for(int i = 0; i < 100; i++) {
        REQUIRE(e->next());
        CHECK(e->columns()[0]->asInt() == (i % 10));
        CHECK(e->columns()[1]->asInt() == (i / 10));
    }*/
}


class ArrayQueryTest : public QueryTest {
protected:
    Retained<Query> query;

    void checkQuery(int docNo, int expectedRowCount) {
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
        CHECK(e->getRowCount() == expectedRowCount);
        while (e->next()) {
            auto cols = e->columns();
            slice docID = cols[0]->asString();
            string expectedDocID = stringWithFormat("rec-%03d", docNo);
            CHECK(docID == slice(expectedDocID));
            ++docNo;
        }
    }

    void testArrayQuery(const string &json, bool checkOptimization) {
        addArrayDocs(1, 90);

        query = store->compileQuery(json);
        string explanation = query->explain();
        Log("%s", explanation.c_str());
        checkQuery(88, 3);

        Log("-------- Repeating with index --------");
        store->createIndex("numbersIndex"_sl,
                           "[[\".numbers\"]]"_sl,
                           KeyStore::kArrayIndex);
        query = store->compileQuery(json);
        explanation = query->explain();
        Log("%s", explanation.c_str());
        if (checkOptimization)
            CHECK(explanation.find("SCAN") == string::npos);    // should be no linear table scans
        checkQuery(88, 3);

        Log("-------- Adding a doc --------");
        addArrayDocs(91, 1);
        checkQuery(88, 4);

        Log("-------- Purging a doc --------");
        deleteDoc("rec-091"_sl, true);
        checkQuery(88, 3);

        Log("-------- Soft-deleting a doc --------");
        deleteDoc("rec-090"_sl, false);
        checkQuery(88, 2);

        Log("-------- Un-deleting a doc --------");
        undeleteDoc("rec-090"_sl);
        checkQuery(88, 3);
    }
};


TEST_CASE_METHOD(ArrayQueryTest, "Query ANY", "[Query]") {
    testArrayQuery(json5("['SELECT', {\
                             WHERE: ['ANY', 'num', ['.numbers'],\
                                            ['=', ['?num'], 'eight-eight']]}]"),
                   false);
}


TEST_CASE_METHOD(ArrayQueryTest, "Query UNNEST", "[Query]") {
    testArrayQuery(json5("['SELECT', {\
                              FROM: [{as: 'doc'}, \
                                     {as: 'num', 'unnest': ['.doc.numbers']}],\
                              WHERE: ['=', ['.num'], 'eight-eight']}]"),
                   true);
}


TEST_CASE_METHOD(QueryTest, "Query NULL check", "[Query]") {
	{
        Transaction t(store->dataFile());
        string docID = "rec-00";

        for(int i = 0; i < 3; i++) {
            stringstream ss(docID);
            ss << i + 1;

            fleece::impl::Encoder enc;
            enc.beginDictionary();
			if(i > 0) {
				enc.writeKey("callsign");
				if(i == 1) {
					enc.writeNull();
				} else {
					enc.writeString("ANA");
				}
			}
            enc.endDictionary();
            alloc_slice body = enc.finish();

            store->set(slice(ss.str()), nullslice, body, DocumentFlags::kNone, t);
        }

        t.commit();
    }

	auto query = store->compileQuery(json5(
        "{'WHAT': [['COUNT()','.'], ['.callsign']], 'WHERE':['IS NOT', ['.callsign'], null]}"));
	unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
	CHECK(e->columns()[1]->asString() == "ANA"_sl);

	query = store->compileQuery(json5(
        "{'WHAT': [['COUNT()','.'], ['.callsign']], 'WHERE':['IS', ['.callsign'], null]}"));
	e.reset(query->createEnumerator());
	REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
	CHECK(e->columns()[1]->asString().buf == nullptr);

	query = store->compileQuery(json5(
        "{'WHAT': [['.callsign']]}"));
	e.reset(query->createEnumerator());
	CHECK(e->getRowCount() == 3); // Make sure there are actually three docs!
}


// NOTE: This test cannot be reproduced in this way on Windows, and it is likely a Unix specific
// problem.  Leaving an enumerator open in this way will cause a permission denied error when
// trying to delete the database via db->deleteDataFile()
#ifndef _MSC_VER
TEST_CASE_METHOD(QueryTest, "Query finalized after db deleted", "[Query]") {
    Retained<Query> query{ store->compileQuery(json5(
          "{WHAT: ['.num', ['*', ['.num'], ['.num']]], WHERE: ['>', ['.num'], 10]}")) };
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    e->next();
    query = nullptr;

    db->deleteDataFile();
    db = nullptr;

    // Now free the query enum, which will free the sqlite_stmt, triggering a SQLite warning
    // callback about the database file being unlinked:
    e.reset();

    // Assert that the callback did not log a warning:
    CHECK(warningsLogged() == 0);
}
#endif
