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
#include <time.h>
#include <float.h>

using namespace fleece::impl;

static string local_to_utc(const char* format, int days, int hours, int minutes,
                               int hourOffset, int minuteOffset) {
    minutes -= minuteOffset;
    hours -= hourOffset;
    if(hours < 0) {
        hours += 24;
        days -= 1;
    } else if (hours > 23) {
        hours -= 24;
        days += 1;
    }
    
    if(minutes < 0) {
        minutes += 60;
        hours -= 1;
    } else if(minutes > 59) {
        minutes -= 60;
        hours += 1;
    }
    
    return stringWithFormat(format, days, hours, minutes);
}

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


TEST_CASE_METHOD(QueryTest, "Query SELECT WHAT", "[Query][N1QL]") {
    addNumberedDocs();
    Retained<Query> query;
    SECTION("JSON") {
        query = store->compileQuery(json5(
            "{WHAT: ['.num', ['AS', ['*', ['.num'], ['.num']], 'square']], WHERE: ['>', ['.num'], 10]}"));
    }
    SECTION("N1QL") {
        query = store->compileQuery("SELECT num, num*num AS square WHERE num > 10"_sl,
                                    QueryLanguage::kN1QL);
    }
    CHECK(query->columnCount() == 2);
    CHECK(query->columnTitles() == (vector<string>{"num", "square"}));
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

    CHECK(query1->columnTitles() == (vector<string>{ "main", "$1" }));

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
        writeDoc("null-and-void"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("n");
            enc.writeNull();
        });
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
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder &enc) {
                enc.writeKey("value");
                enc.writeBool(i == 0);
            });
        }
        
        // Integer 0 and 1 would have fooled ISBOOLEAN() before
        for(int i = 2; i < 4; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder &enc) {
                enc.writeKey("value");
                enc.writeInt(i - 2);
            });
        }
        
        t.commit();
    }
    
    Retained<Query> query{ store->compileQuery(json5(
        "{WHAT: ['._id'], WHERE: ['ISBOOLEAN()', ['.value']]}")) };
    CHECK(query->columnTitles() == (vector<string>{"id"}));
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
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("$foo");    enc.writeInt(17);
            enc.writeKey("?foo");    enc.writeInt(18);
            enc.writeKey("[foo");    enc.writeInt(19);
            enc.writeKey(".foo");    enc.writeInt(20);
            enc.writeKey("f$o?o[o."); enc.writeInt(21);
            enc.writeKey("oids");
                enc.beginArray();
                    enc.beginDictionary();
                        enc.writeKey("$oid");   enc.writeString("avoid");
                    enc.endDictionary();
                enc.endArray();
        });
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

    // Final boss:
    CHECK(rowsInQuery(json5("{WHERE: ['ANY', 'a', ['.oids'],\
                                           ['=', ['?a.$oid'], 'avoid']]}"))
          == 1);

}


TEST_CASE_METHOD(QueryTest, "Query object properties", "[Query]") {
    {
        Transaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }

    Retained<Query> query{ store->compileQuery(json5("['=', 'FTW', ['_.subvalue', ['.value']]]")) };

    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "doc4"_sl);
    CHECK(!e->next());
}


TEST_CASE_METHOD(QueryTest, "Query array index", "[Query]") {
    addArrayDocs();

    Retained<Query> query{ store->compileQuery(json5("['=', 'five', ['_.[0]', ['.numbers']]]")) };

    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    int i = 0;
    while (e->next()) {
        slice docID = e->columns()[0]->asString();
        CHECK(docID == "rec-010"_sl);
        ++i;
    }
    REQUIRE(i == 1);
}


TEST_CASE_METHOD(QueryTest, "Query array literal", "[Query]") {
    addNumberedDocs(1, 1);
    Retained<Query> query{ store->compileQuery(json5(
        "{WHAT: [['[]', null, false, true, 12345, 1234.5, 'howdy', ['._id']]]}")) };

    CHECK(query->columnTitles() == (vector<string>{"$1"}));

    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSONString() == "[null,false,true,12345,1234.5,\"howdy\",\"rec-001\"]");
    CHECK(!e->next());
}


TEST_CASE_METHOD(QueryTest, "Query dict literal", "[Query]") {
    addNumberedDocs(1, 1);
    Retained<Query> query{ store->compileQuery(json5(
        "{WHAT: [{n: null, f: false, t: true, i: 12345, d: 1234.5, s: 'howdy', m: ['.bogus'], id: ['._id']}]}")) };

    CHECK(query->columnTitles() == (vector<string>{"$1"}));

    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSON<5>() == "{d:1234.5,f:false,i:12345,id:\"rec-001\",n:null,s:\"howdy\",t:true}"_sl);
    CHECK(!e->next());
}


TEST_CASE_METHOD(QueryTest, "Query dict literal with blob", "[Query]") {
    // Create a doc with a blob property:
    {
        Transaction t(store->dataFile());
        writeDoc("goop"_sl, (DocumentFlags)0, t, [](Encoder &enc) {
            enc.writeKey("text");
            enc.beginDictionary();
            enc.writeKey("@type"); enc.writeString("blob");
            enc.writeKey("digest"); enc.writeString("xxxxxxx");
            enc.endDictionary();
        });
        t.commit();
    }

    Retained<Query> query{ store->compileQuery(json5(
                  "{WHAT:[ ['.text'], {'text':['.text']} ]}")) };
    Log("%s", query->explain().c_str());
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    auto d = e->columns()[0]->asDict();
    REQUIRE(d);
    CHECK(d->toJSON(true) == "{\"@type\":\"blob\",\"digest\":\"xxxxxxx\"}"_sl);
    REQUIRE(!e->next());
}


#pragma mark Targeted N1QL tests
    
TEST_CASE_METHOD(QueryTest, "Query array length", "[Query]") {
    {
        Transaction t(store->dataFile());
        for(int i = 0; i < 2; i++) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder &enc) {
                enc.writeKey("value");
                enc.beginArray(i + 1);
                for(int j = 0; j < i + 1; j++) {
                    enc.writeInt(j);
                }
                enc.endArray();
            });
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
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeNull();
            enc.writeKey("real_value");
            enc.writeInt(1);
        });
        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeNull();
            enc.writeKey("atai");
            enc.writeInt(1);
        });
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
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeString("awesome value");
        });
        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeString("cool value");
        });
        writeDoc("doc3"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("value");
            enc.writeString("invalid");
        });
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
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder &enc) {
                enc.writeKey("identifier");
                enc.writeInt(i >= 5 ? i >= 15 ? 3 : 2 : 1);
                enc.writeKey("index");
                enc.writeInt(i);
            });
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


#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(QueryTest, "Query Distance Metrics", "[Query]") {
    testExpressions( {
        {"['euclidean_distance()', ['[]', 10, 10], ['[]', 13, 14]]",    "5"},
        {"['euclidean_distance()', ['[]', 10, 10], ['[]', 13, 14], 2]", "25"},
        {"['euclidean_distance()', ['[]', 1,2,3,4,5], ['[]', 1,2,3,4,5]]","0"},
        {"['euclidean_distance()', ['[]'], ['[]']]",                    "0"},
        {"['euclidean_distance()', 18, 'foo']",                         "null"},
        {"['euclidean_distance()', ['[]', 10, 10], ['[]', 13]]",        "null"},

        {"['cosine_distance()', ['[]', 10, 0], ['[]', 0, 99]]",         "1"},
        {"['cosine_distance()', ['[]', 1,2,3,4,5], ['[]', 1,2,3,4,5]]", "0"},
        {"['cosine_distance()', ['[]'], ['[]']]",                       "null"},
        {"['cosine_distance()', 18, 'foo']",                            "null"},
        {"['cosine_distance()', ['[]', 10, 10], ['[]', 13]]",           "null"},
    } );
}
#endif


TEST_CASE_METHOD(QueryTest, "Query Date Functions", "[Query]") {
    // Calculate offset
    time_t rawtime = 1540252800; // 2018-10-23 midnight GMT
    struct tm gbuf;
    struct tm lbuf;
    gmtime_r(&rawtime, &gbuf);
    localtime_r(&rawtime, &lbuf);
    time_t gmt = mktime(&gbuf);
    auto diff = (int)difftime(rawtime, gmt);
    if(lbuf.tm_isdst > 0) {
        // mktime uses GMT, but we want UTC which is unaffected
        // by DST
        diff += 3600;
    }
    
    auto diffTotal = (int)(diff / 60.0);
    auto diffHour = (int)(diffTotal / 60.0);
    auto diffMinute = diffTotal % 60;
    
    auto expected1 = local_to_utc("2018-10-%02dT%02d:%02d:00Z", 23, 0, 0, diffHour, diffMinute);
    auto expected2 = local_to_utc("2018-10-%02dT%02d:%02d:00Z", 23, 18, 33, diffHour, diffMinute);
    auto expected3 = local_to_utc("2018-10-%02dT%02d:%02d:01Z", 23, 18, 33, diffHour, diffMinute);
    
    testExpressions( {
        {"['str_to_utc()', null]",                            "null"},
        {"['str_to_utc()', 99]",                              "null"},
        {"['str_to_utc()', '']",                              "null"},
        {"['str_to_utc()', 'x']",                             "null"},
        {"['str_to_utc()', '2018-10-23']",                    expected1},
        {"['str_to_utc()', '2018-10-23T18:33']",              expected2},
        {"['str_to_utc()', '2018-10-23T18:33:01']",           expected3},
        {"['str_to_utc()', '2018-10-23T18:33:01Z']",          "2018-10-23T18:33:01Z"},
        {"['str_to_utc()', '2018-10-23T11:33:01-0700']",      "2018-10-23T18:33:01Z"},
        {"['str_to_utc()', '2018-10-23T11:33:01+03:30']",     "2018-10-23T08:03:01Z"},
        {"['str_to_utc()', '2018-10-23T18:33:01.123Z']",      "2018-10-23T18:33:01.123Z"},
        {"['str_to_utc()', '2018-10-23T11:33:01.123-0700']",  "2018-10-23T18:33:01.123Z"},

        {"['str_to_millis()', '']",                           "null"},
        {"['str_to_millis()', '1970-01-01T00:00:00Z']",       "0"},
        {"['str_to_millis()', '2018-10-23T11:33:01-0700']",   "1540319581000"},
        {"['str_to_millis()', '2018-10-23T18:33:01Z']",       "1540319581000"},
        {"['str_to_millis()', '2018-10-23T18:33:01.123Z']",   "1540319581123"},

        // Range check the month and day number
        {"['str_to_millis()', '2000-00-01T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2000-13-01T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2000-01-00T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2000-01-32T00:00:00Z']",       "null"},

        // 30 days hath September...
        {"['str_to_millis()', '2018-01-31T00:00:00Z']",       "1517356800000"},
        {"['str_to_millis()', '2018-02-31T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2018-03-31T00:00:00Z']",       "1522454400000"},
        {"['str_to_millis()', '2018-04-31T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2018-05-31T00:00:00Z']",       "1527724800000"},
        {"['str_to_millis()', '2018-06-31T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2018-07-31T00:00:00Z']",       "1532995200000"},
        {"['str_to_millis()', '2018-08-31T00:00:00Z']",       "1535673600000"},
        {"['str_to_millis()', '2018-09-31T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2018-10-31T00:00:00Z']",       "1540944000000"},
        {"['str_to_millis()', '2018-11-31T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2018-12-31T00:00:00Z']",       "1546214400000"},

        // February is complicated
        {"['str_to_millis()', '2000-02-29T00:00:00Z']",       "951782400000"},
        {"['str_to_millis()', '2016-02-29T00:00:00Z']",       "1456704000000"},
        {"['str_to_millis()', '2018-02-29T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2100-02-29T00:00:00Z']",       "null"},
        {"['str_to_millis()', '2400-02-29T00:00:00Z']",       "13574563200000"},
        {"['str_to_millis()', '2400-02-30T00:00:00Z']",       "null"},

        {"['millis_to_utc()', 'x']",                          "null"},
        {"['millis_to_utc()', '0']",                          "null"},
        {"['millis_to_utc()', 0]",                            "1970-01-01T00:00:00Z"},
        {"['millis_to_utc()', 1540319581000]",                "2018-10-23T18:33:01Z"},
        {"['millis_to_utc()', 1540319581123]",                "2018-10-23T18:33:01.123Z"},
        {"['millis_to_utc()', 1540319581999]",                "2018-10-23T18:33:01.999Z"},

        // It's hard to test millis_to_str directly, because the result depends on the
        // local time zone...
        //{"['millis_to_str()', 1540319581000]", "2018-10-23T11:33:01-0700"},
        {"['str_to_utc()', ['millis_to_str()', 1540319581000]]", "2018-10-23T18:33:01Z"},
        {"['millis_to_str()', 'x']",                          "null"},
        {"['millis_to_str()', '0']",                          "null"},
    } );
}

TEST_CASE_METHOD(QueryTest, "Query date diff string", "[Query]") {
    SECTION("Basic") {
        testExpressions( {
            {"['date_diff_str()', '2018-01-31T00:00:00.001Z', '2018-01-31T00:00:00Z', 'millisecond']", int64_t(1ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00.010Z', '2018-01-31T00:00:00Z', 'millisecond']", int64_t(10ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00.100Z', '2018-01-31T00:00:00Z', 'millisecond']", int64_t(100ll)},
            {"['date_diff_str()', '2018-01-31T00:00:01.5Z', '2018-01-31T00:00:00Z', 'millisecond']", int64_t(1500ll)},
            {"['date_diff_str()', '2018-01-31T00:00:01.5Z', '2018-01-31T00:00:00Z', 'second']", int64_t(1ll)},
            {"['date_diff_str()', '2018-01-31T00:01:01.5Z', '2018-01-31T00:00:00Z', 'second']", int64_t(61ll)},
            {"['date_diff_str()', '2018-01-31T00:01:01.5Z', '2018-01-31T00:00:00Z', 'minute']", int64_t(1ll)},
            {"['date_diff_str()', '2018-01-31T01:01:00.5Z', '2018-01-31T00:00:00Z', 'minute']", int64_t(61ll)},
            {"['date_diff_str()', '2018-01-31T01:00:01.5Z', '2018-01-31T00:00:00Z', 'hour']", int64_t(1ll)},
            {"['date_diff_str()', '2018-02-01T01:00:00.5Z', '2018-01-31T00:00:00Z', 'hour']", int64_t(25ll)},
            {"['date_diff_str()', '2018-01-02T01:00:01.5Z', 1514764800000, 'day']", int64_t(1ll)},
            {"['date_diff_str()', '2018-03-01T01:00:01.5Z', '2018-02-01T00:00:00Z', 'day']", int64_t(28ll)},
            {"['date_diff_str()', '2016-03-01T01:00:01.5Z', '2016-02-01T00:00:00Z', 'day']", int64_t(29ll)},
            {"['date_diff_str()', '2018-02-01T01:00:00.5Z', 1514764800000, 'day']", int64_t(31ll)},
            {"['date_diff_str()', '2018-01-01T01:00:01.5Z', '2017-01-01T00:00:00Z', 'day']", int64_t(365ll)},
            {"['date_diff_str()', '2017-01-01T01:00:01.5Z', '2016-01-01T00:00:00Z', 'day']", int64_t(366ll)},
            {"['date_diff_str()', '2017-01-08T01:00:01.5Z', '2017-01-01T00:00:00Z', 'week']", int64_t(1ll)},
            {"['date_diff_str()', '2017-02-01T01:00:01.5Z', '2017-01-01T00:00:00Z', 'week']", int64_t(4ll)},
            {"['date_diff_str()', '2017-02-08T01:00:01.5Z', '2017-01-01T00:00:00Z', 'month']", int64_t(1ll)},
            {"['date_diff_str()', '2018-02-01T01:00:01.5Z', '2017-01-01T00:00:00Z', 'month']", int64_t(13ll)},
            {"['date_diff_str()', '2017-04-08T01:00:01.5Z', '2017-01-01T00:00:00Z', 'quarter']", int64_t(1ll)},
            {"['date_diff_str()', '2018-04-01T01:00:01.5Z', '2017-01-01T00:00:00Z', 'quarter']", int64_t(5ll)},
            {"['date_diff_str()', '2018-04-08T01:00:01.5Z', '2017-01-01T00:00:00Z', 'year']", int64_t(1ll)},
            {"['date_diff_str()', '2028-04-01T01:00:01.5Z', '2017-01-01T00:00:00Z', 'decade']", int64_t(1ll)},
            {"['date_diff_str()', '2118-04-08T01:00:01.5Z', '2017-01-01T00:00:00Z', 'century']", int64_t(1ll)},

            // NOTE: Windows cannot handle higher than year 3000
            {"['date_diff_str()', '2918-04-01T01:00:01.5Z', '1917-01-01T00:00:00Z', 'millennium']", int64_t(1ll)},

        });
    }

    SECTION("Negative") {
        testExpressions( {
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:00.001Z', 'millisecond']", int64_t(-1ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:00.010Z', 'millisecond']", int64_t(-10ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:00.100Z', 'millisecond']", int64_t(-100ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:01.5Z', 'second']", int64_t(-1ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:01:01.5Z', 'second']", int64_t(-61ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:01:01.5Z', 'minute']", int64_t(-1ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T01:01:00.5Z', 'minute']", int64_t(-61ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T01:00:01.5Z', 'hour']", int64_t(-1ll)},
            {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-02-01T01:00:00.5Z', 'hour']", int64_t(-25ll)},
            {"['date_diff_str()', 1514764800000, '2018-01-02T01:00:01.5Z', 'day']", int64_t(-1ll)},
            {"['date_diff_str()', '2018-02-01T00:00:00Z', '2018-03-01T01:00:01.5Z', 'day']", int64_t(-28ll)},
            {"['date_diff_str()', '2016-02-01T00:00:00Z', '2016-03-01T01:00:01.5Z', 'day']", int64_t(-29ll)},
            {"['date_diff_str()', 1514764800000, '2018-02-01T01:00:00.5Z', 'day']", int64_t(-31ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2018-01-01T01:00:01.5Z', 'day']", int64_t(-365ll)},
            {"['date_diff_str()', '2016-01-01T00:00:00Z', '2017-01-01T01:00:01.5Z', 'day']", int64_t(-366ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2017-01-08T01:00:01.5Z', 'week']", int64_t(-1ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2017-02-01T01:00:01.5Z', 'week']", int64_t(-4ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2017-02-08T01:00:01.5Z', 'month']", int64_t(-1ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2018-02-01T01:00:01.5Z', 'month']", int64_t(-13ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2017-04-08T01:00:01.5Z', 'quarter']", int64_t(-1ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2018-04-01T01:00:01.5Z', 'quarter']", int64_t(-5ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2018-04-08T01:00:01.5Z', 'year']", int64_t(-1ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2028-04-01T01:00:01.5Z', 'decade']", int64_t(-1ll)},
            {"['date_diff_str()', '2017-01-01T00:00:00Z', '2118-04-08T01:00:01.5Z', 'century']", int64_t(-1ll)},

            // NOTE: Windows cannot handle higher than year 3000
            {"['date_diff_str()', '1917-01-01T00:00:00Z', '2918-04-01T01:00:01.5Z', 'millennium']", int64_t(-1ll)},

        });
    }

    SECTION("N1QL consistency") {
        testExpressions( {
            // https://github.com/couchbase/query/blob/master/test/filestore/test_cases/date_functions/case_func_date.json
            {"['date_diff_str()', '2006-01-02', '1998-02-02', 'year']", int64_t(8ll)},
            {"['date_diff_str()', '2014-12-01','2015-01-01', 'quarter']", int64_t(-1ll)},
            {"['date_diff_str()', '2015-01-01','2014-12-01', 'quarter']", int64_t(1ll)},
            {"['date_diff_str()', '2013-12-01','2015-01-01', 'quarter']", int64_t(-5ll)},
            {"['date_diff_str()', '2013-10-01','2015-01-01', 'quarter']", int64_t(-5ll)},
            {"['date_diff_str()', '2014-12-01','2015-05-30', 'quarter']", int64_t(-2ll)},
            {"['date_diff_str()', '2014-10-01','2014-12-01', 'quarter']", int64_t(0ll)},
            {"['date_diff_str()', '2015-11-01','2014-10-01', 'month']", int64_t(13ll)},
            {"['date_diff_str()', '2015-01-01','2014-12-01', 'month']", int64_t(1ll)},
            {"['date_diff_str()', '2013-12-01','2015-01-01', 'month']", int64_t(-13ll)},
            {"['date_diff_str()', '2013-01-01','2015-01-01', 'month']", int64_t(-24ll)},
            {"['date_diff_str()', '2013-10-01','2015-01-01', 'month']", int64_t(-15ll)},
            {"['date_diff_str()', '2014-12-01','2015-01-01', 'month']", int64_t(-1ll)},
            {"['date_diff_str()', '2018-09-10 23:59:59','2018-09-11 00:00:01','day']", int64_t(-1ll)},
            {"['date_diff_str()', '2018-09-11 00:00:01','2018-09-10 23:59:59','day']", int64_t(1ll)},

            // Give unintuitive results on purpose for the sake of consistency
            {"['date_diff_str()', '2018-01-31T00:01:01.5Z', '2018-01-31T00:01:00.9Z', 'second']", int64_t(1ll)},
            {"['date_diff_str()', '2018-01-31T00:02:01Z', '2018-01-31T00:01:59Z', 'minute']", int64_t(1ll)},
            {"['date_diff_str()', '2018-01-31T01:00:00Z', '2018-01-31T00:59:00Z', 'hour']", int64_t(1ll)},
            {"['date_diff_str()', '2018-02-01T00:01:59Z', '2018-01-31T23:59:01Z', 'day']", int64_t(1ll)},
            {"['date_diff_str()', '2018-02-01T00:01:59Z', '2018-01-31T23:59:01Z', 'month']", int64_t(1ll)},
            {"['date_diff_str()', '2018-01-01T00:01:59Z', '2017-12-31T23:59:01Z', 'year']", int64_t(1ll)},
            {"['date_diff_str()', '2018-01-31T02:00:00-07:00', '2018-01-31T00:00:00-08:00', 'hour']", int64_t(2ll)},
        });
    }
}

TEST_CASE_METHOD(QueryTest, "Query date diff millis", "[Query]") {
    SECTION("Basic") {
        testExpressions( {
            {"['date_diff_millis()', 1517356800001, 1517356800000, 'millisecond']", int64_t(1ll)},
            {"['date_diff_millis()', 1517356800010, 1517356800000, 'millisecond']", int64_t(10ll)},
            {"['date_diff_millis()', 1517356800100, 1517356800000, 'millisecond']", int64_t(100ll)},
            {"['date_diff_millis()', 1517356801500, 1517356800000, 'millisecond']", int64_t(1500ll)},
            {"['date_diff_millis()', 1517356801500, 1517356800000, 'second']", int64_t(1ll)},
            {"['date_diff_millis()', 1517356861500, 1517356800000, 'second']", int64_t(61ll)},
            {"['date_diff_millis()', 1517356861500, 1517356800000, 'minute']", int64_t(1ll)},
            {"['date_diff_millis()', 1517360461500, 1517356800000, 'minute']", int64_t(61ll)},
            {"['date_diff_millis()', 1517360401500, 1517356800000, 'hour']", int64_t(1ll)},
            {"['date_diff_millis()', 1517446800500, 1517356800000, 'hour']", int64_t(25ll)},
            {"['date_diff_millis()', 1514854801000, 1514764800000, 'day']", int64_t(1ll)},
            {"['date_diff_millis()', 1519866001500, 1517443200000, 'day']", int64_t(28ll)},
            {"['date_diff_millis()', 1456794001500, 1454284800000, 'day']", int64_t(29ll)},
            {"['date_diff_millis()', 1517446800500, 1514764800000, 'day']", int64_t(31ll)},
            {"['date_diff_millis()', 1514768401500, 1483228800000, 'day']", int64_t(365ll)},
            {"['date_diff_millis()', 1483232401500, 1451606400000, 'day']", int64_t(366ll)},
            {"['date_diff_millis()', 1483837201000, 1483228800000, 'week']", int64_t(1ll)},
            {"['date_diff_millis()', 1485910801500, 1483228800000, 'week']", int64_t(4ll)},
            {"['date_diff_millis()', 1486515601500, 1483228800000, 'month']", int64_t(1ll)},
            {"['date_diff_millis()', 1517446801500, 1483228800000, 'month']", int64_t(13ll)},
            {"['date_diff_millis()', 1491613201500, 1483228800000, 'quarter']", int64_t(1ll)},
            {"['date_diff_millis()', 1522544401500, 1483228800000, 'quarter']", int64_t(5ll)},
            {"['date_diff_millis()', 1523149201000, 1483228800000, 'year']", int64_t(1ll)},
            {"['date_diff_millis()', 1838163601500, 1483228800000, 'decade']", int64_t(1ll)},
            {"['date_diff_millis()', 4678822801500, 1483228800000, 'century']", int64_t(1ll)},

            // NOTE: Windows cannot handle higher than year 3000
            {"['date_diff_millis()', 29923779601500, -1672531200000, 'millennium']", int64_t(1ll)}
        });
    }

    SECTION("Negative") {
       testExpressions( {
            {"['date_diff_millis()', 1517356800000, 1517356800001, 'millisecond']", int64_t(-1ll)},
            {"['date_diff_millis()', 1517356800000, 1517356800010, 'millisecond']", int64_t(-10ll)},
            {"['date_diff_millis()', 1517356800000, 1517356800100, 'millisecond']", int64_t(-100ll)},
            {"['date_diff_millis()', 1517356800000, 1517356801500, 'millisecond']", int64_t(-1500ll)},
            {"['date_diff_millis()', 1517356800000, 1517356801500, 'second']", int64_t(-1ll)},
            {"['date_diff_millis()', 1517356800000, 1517356861500, 'second']", int64_t(-61ll)},
            {"['date_diff_millis()', 1517356800000, 1517356861500, 'minute']", int64_t(-1ll)},
            {"['date_diff_millis()', 1517356800000, 1517360461500, 'minute']", int64_t(-61ll)},
            {"['date_diff_millis()', 1517356800000, 1517360401500, 'hour']", int64_t(-1ll)},
            {"['date_diff_millis()', 1517356800000, 1517446800500, 'hour']", int64_t(-25ll)},
            {"['date_diff_millis()', 1514764800000, 1514854801000, 'day']", int64_t(-1ll)},
            {"['date_diff_millis()', 1517443200000, 1519866001500, 'day']", int64_t(-28ll)},
            {"['date_diff_millis()', 1454284800000, 1456794001500, 'day']", int64_t(-29ll)},
            {"['date_diff_millis()', 1514764800000, 1517446800500, 'day']", int64_t(-31ll)},
            {"['date_diff_millis()', 1483228800000, 1514768401500, 'day']", int64_t(-365ll)},
            {"['date_diff_millis()', 1451606400000, 1483232401500, 'day']", int64_t(-366ll)},
            {"['date_diff_millis()', 1483228800000, 1483837201000, 'week']", int64_t(-1ll)},
            {"['date_diff_millis()', 1483228800000, 1485910801500, 'week']", int64_t(-4ll)},
            {"['date_diff_millis()', 1483228800000, 1486515601500, 'month']", int64_t(-1ll)},
            {"['date_diff_millis()', 1483228800000, 1517446801500, 'month']", int64_t(-13ll)},
            {"['date_diff_millis()', 1483228800000, 1491613201500, 'quarter']", int64_t(-1ll)},
            {"['date_diff_millis()', 1483228800000, 1522544401500, 'quarter']", int64_t(-5ll)},
            {"['date_diff_millis()', 1483228800000, 1523149201000, 'year']", int64_t(-1ll)},
            {"['date_diff_millis()', 1483228800000, 1838163601500, 'decade']", int64_t(-1ll)},
            {"['date_diff_millis()', 1483228800000, 4678822801500, 'century']", int64_t(-1ll)},

            // NOTE: Windows cannot handle higher than year 3000
            {"['date_diff_millis()', -1672531200000, 29923779601500, 'millennium']", int64_t(-1ll)}
        });
    }

    SECTION("N1QL consistency") {
        testExpressions( {
            // https://github.com/couchbase/query/blob/master/test/filestore/test_cases/date_functions/case_func_date.json
            {"['date_diff_millis()', 1136160000000,886377600000, 'year']", int64_t(8ll)},
            {"['date_diff_millis()', 1417392000000,1420070400000, 'quarter']", int64_t(-1ll)},
            {"['date_diff_millis()', 1420070400000,1417392000000, 'quarter']", int64_t(1ll)},
            {"['date_diff_millis()', 1385856000000,1420070400000, 'quarter']", int64_t(-5ll)},
            {"['date_diff_millis()', 1380585600000,1420070400000, 'quarter']", int64_t(-5ll)},
            {"['date_diff_millis()', 1417392000000,1432944000000, 'quarter']", int64_t(-2ll)},
            {"['date_diff_millis()', 1412121600000,1417392000000, 'quarter']", int64_t(0ll)},
            {"['date_diff_millis()', 1446336000000,1412121600000, 'month']", int64_t(13ll)},
            {"['date_diff_millis()', 1420070400000,1417392000000, 'month']", int64_t(1ll)},
            {"['date_diff_millis()', 1385856000000,1420070400000, 'month']", int64_t(-13ll)},
            {"['date_diff_millis()', 1356998400000,1420070400000, 'month']", int64_t(-24ll)},
            {"['date_diff_millis()', 1380585600000,1420070400000, 'month']", int64_t(-15ll)},
            {"['date_diff_millis()', 1417392000000,1420070400000, 'month']", int64_t(-1ll)},
            {"['date_diff_millis()', 1536623999000,1536624001000,'day']", int64_t(-1ll)},
            {"['date_diff_millis()', 1536624001000,1536623999000,'day']", int64_t(1ll)},

            // Give unintuitive results on purpose for the sake of consistency
            {"['date_diff_millis()', 1517356861500, 1517356860900, 'second']", int64_t(1ll)},
            {"['date_diff_millis()', 1517356921000, 1517356919000, 'minute']", int64_t(1ll)},
            {"['date_diff_millis()', 1517360400000, 1517360340000, 'hour']", int64_t(1ll)},
            {"['date_diff_millis()', 1517443319000, 1517443141000, 'day']", int64_t(1ll)},
            {"['date_diff_millis()', 1517443319000, 1517443141000, 'month']", int64_t(1ll)},
            {"['date_diff_millis()', 1514764919000, 1514764741000, 'year']", int64_t(1ll)},
        });
    }
}

TEST_CASE_METHOD(QueryTest, "Query date add string", "[Query]") {
    SECTION("Basic") {
        testExpressions( {
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'millisecond']", "2018-01-01T00:00:00.001Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 10, 'millisecond']", "2018-01-01T00:00:00.010Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 100, 'millisecond']", "2018-01-01T00:00:00.100Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'second']", "2018-01-01T00:00:01Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'minute']", "2018-01-01T00:01:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'hour']", "2018-01-01T01:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'day']", "2018-01-02T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'week']", "2018-01-08T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'month']", "2018-02-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'quarter']", "2018-04-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'year']", "2019-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'decade']", "2028-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1, 'century']", "2118-01-01T00:00:00Z"},

            // Note: Windows cannot handle times after year 3000
            {"['date_add_str()', '1918-01-01T00:00:00Z', 1, 'millennium']", "2918-01-01T00:00:00Z"},
        });
    }

    SECTION("Negative") {
        testExpressions( {
            {"['date_add_str()', '2018-01-01T00:00:00.001Z', -1, 'millisecond']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00.010Z', -10, 'millisecond']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00.100Z', -100, 'millisecond']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:01Z', -1, 'second']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:01:00Z', -1, 'minute']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T01:00:00Z', -1, 'hour']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-02T00:00:00Z', -1, 'day']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-08T00:00:00Z', -1, 'week']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-02-01T00:00:00Z', -1, 'month']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-04-01T00:00:00Z', -1, 'quarter']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2019-01-01T00:00:00Z', -1, 'year']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2028-01-01T00:00:00Z', -1, 'decade']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2118-01-01T00:00:00Z', -1, 'century']", "2018-01-01T00:00:00Z"},

            // Note: Windows cannot handle times before year 1970
            {"['date_add_str()', '2970-01-01T00:00:00Z', -1, 'millennium']", "1970-01-01T00:00:00Z"},
        });
    }

    SECTION("Overflow") {
        testExpressions( {
            {"['date_add_str()', '2018-01-01T00:00:00Z', 1500, 'millisecond']", "2018-01-01T00:00:01.500Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 61, 'second']", "2018-01-01T00:01:01Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 61, 'minute']", "2018-01-01T01:01:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 25, 'hour']", "2018-01-02T01:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 31, 'day']", "2018-02-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:00Z', 12, 'month']", "2019-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:00:01.500Z', -1500, 'millisecond']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T00:01:01Z', -61, 'second']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-01T01:01:00Z', -61, 'minute']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-01-02T01:00:00Z', -25, 'hour']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2018-02-01T00:00:00Z', -31, 'day']", "2018-01-01T00:00:00Z"},
            {"['date_add_str()', '2019-01-01T00:00:00Z', -12, 'month']", "2018-01-01T00:00:00Z"}
        });
    }

    SECTION("Special cases") {
        testExpressions( {
            {"['date_add_str()', '2018-02-28T00:00:00Z', 1, 'day']", "2018-03-01T00:00:00Z"},
            {"['date_add_str()', '2016-02-28T00:00:00Z', 1, 'day']", "2016-02-29T00:00:00Z"},
            {"['date_add_str()', '2016-01-01T00:00:00-07:00', 1, 'day']", "2016-01-02T00:00:00-07:00"},

            // Questionable result, but matches N1QL server
            {"['date_add_str()', '2018-01-31T00:00:00Z', 1, 'month']", "2018-03-03T00:00:00Z"},
        });
    }
}

TEST_CASE_METHOD(QueryTest, "Query date add millis", "[Query]") {
     SECTION("Basic") {
        testExpressions( {
            {"['date_add_millis()', 1514764800000, 1, 'millisecond']", int64_t(1514764800001)},
            {"['date_add_millis()', 1514764800000, 10, 'millisecond']", int64_t(1514764800010)},
            {"['date_add_millis()', 1514764800000, 100, 'millisecond']", int64_t(1514764800100)},
            {"['date_add_millis()', 1514764800000, 1, 'second']", int64_t(1514764801000)},
            {"['date_add_millis()', 1514764800000, 1, 'minute']", int64_t(1514764860000)},
            {"['date_add_millis()', 1514764800000, 1, 'hour']", int64_t(1514768400000)},
            {"['date_add_millis()', 1514764800000, 1, 'day']", int64_t(1514851200000)},
            {"['date_add_millis()', 1514764800000, 1, 'week']", int64_t(1515369600000)},
            {"['date_add_millis()', 1514764800000, 1, 'month']", int64_t(1517443200000)},
            {"['date_add_millis()', 1514764800000, 1, 'quarter']", int64_t(1522540800000)},
            {"['date_add_millis()', 1514764800000, 1, 'year']", int64_t(1546300800000)},
            {"['date_add_millis()', 1514764800000, 1, 'decade']", int64_t(1830297600000)},
            {"['date_add_millis()', 1514764800000, 1, 'century']", int64_t(4670438400000)},

            // Note: Windows cannot handle negative timestamps
            {"['date_add_millis()', 0, 1, 'millennium']", int64_t(31556995200000)},
        });
    }

    SECTION("Negative") {
        testExpressions( {
            {"['date_add_millis()', 1514764800001, -1, 'millisecond']", int64_t(1514764800000)},
            {"['date_add_millis()', 1514764800010, -10, 'millisecond']", int64_t(1514764800000)},
            {"['date_add_millis()', 1514764800100, -100, 'millisecond']", int64_t(1514764800000)},
            {"['date_add_millis()', 1514764801000, -1, 'second']", int64_t(1514764800000)},
            {"['date_add_millis()', 1514764860000, -1, 'minute']", int64_t(1514764800000)},
            {"['date_add_millis()', 1514768400000, -1, 'hour']", int64_t(1514764800000)},
            {"['date_add_millis()', 1514851200000, -1, 'day']", int64_t(1514764800000)},
            {"['date_add_millis()', 1515369600000, -1, 'week']", int64_t(1514764800000)},
            {"['date_add_millis()', 1517443200000, -1, 'month']", int64_t(1514764800000)},
            {"['date_add_millis()', 1522540800000, -1, 'quarter']", int64_t(1514764800000)},
            {"['date_add_millis()', 1546300800000, -1, 'year']", int64_t(1514764800000)},
            {"['date_add_millis()', 1830297600000, -1, 'decade']", int64_t(1514764800000)},
            {"['date_add_millis()', 4670438400000, -1, 'century']", int64_t(1514764800000)},

            // Note: Windows cannot handle times before year 1970
            {"['date_add_millis()', 31556995200000, -1, 'millennium']", int64_t(0)}
        });
    }
}

TEST_CASE_METHOD(QueryTest, "Query unsigned", "[Query]") {
    {
        Transaction t(store->dataFile());
        writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("num");
            enc.writeUInt(1);
        });
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
         writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
             enc.writeKey("num");
             enc.writeData("number one"_sl);
         });
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
        writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("num");
            enc.writeInt(1234);
            enc.writeKey("string");
            enc.writeString("FOO");
        });
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
        writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("num");
            enc.writeInt(1234);
            enc.writeKey("string");
            enc.writeString("FOO");
        });
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

            writeDoc(slice(ss.str()), DocumentFlags::kNone, t, [=](Encoder &enc) {
                enc.writeKey("num1");
                enc.writeInt(i);
                enc.writeKey("num2");
                enc.writeInt(10 - i);
            });
        }

         writeDoc("magic"_sl, DocumentFlags::kNone, t, [=](Encoder &enc) {
             enc.writeKey("theone");
             enc.writeInt(4);
         });

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

        Log("-------- Creating index --------");
        store->createIndex("numbersIndex"_sl,
                           "[[\".numbers\"]]"_sl,
                           KeyStore::kArrayIndex);
        Log("-------- Recompiling query with index --------");
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


TEST_CASE_METHOD(ArrayQueryTest, "Query ANY expression", "[Query]") {
    addArrayDocs(1, 90);

    auto json = json5("['SELECT', {\
                          WHERE: ['ANY', 'num', ['[]', ['.numbers[0]'], ['.numbers[1]']],\
                                         ['=', ['?num'], 'eight']]}]");
    query = store->compileQuery(json);
    string explanation = query->explain();
    Log("%s", explanation.c_str());

    checkQuery(12, 2);
}


TEST_CASE_METHOD(ArrayQueryTest, "Query UNNEST expression", "[Query]") {
    addArrayDocs(1, 90);

    auto json = json5("['SELECT', {\
                              FROM: [{as: 'doc'}, \
                                     {as: 'num', 'unnest': ['[]', ['.doc.numbers[0]'], ['.doc.numbers[1]']]}],\
                              WHERE: ['=', ['.num'], 'one-eight']}]");
    query = store->compileQuery(json);
    string explanation = query->explain();
    Log("%s", explanation.c_str());

    checkQuery(22, 2);

    Log("-------- Creating index --------");
    store->createIndex("numbersIndex"_sl,
                       json5("[['[]', ['.numbers[0]'], ['.numbers[1]']]]"),
                       KeyStore::kArrayIndex);
    Log("-------- Recompiling query with index --------");
    query = store->compileQuery(json);
    explanation = query->explain();
    Log("%s", explanation.c_str());
    CHECK(explanation.find("SCAN") == string::npos);    // should be no linear table scans

    checkQuery(22, 2);
}

TEST_CASE_METHOD(QueryTest, "Query NULL check", "[Query]") {
	{
        Transaction t(store->dataFile());
        string docID = "rec-00";

        for(int i = 0; i < 3; i++) {
            stringstream ss(docID);
            ss << i + 1;
            writeDoc(slice(ss.str()), DocumentFlags::kNone, t, [=](Encoder &enc) {
                if(i > 0) {
                    enc.writeKey("callsign");
                    if(i == 1) {
                        enc.writeNull();
                    } else {
                        enc.writeString("ANA");
                    }
                }
            });
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

    deleteDatabase();

    // Now free the query enum, which will free the sqlite_stmt, triggering a SQLite warning
    // callback about the database file being unlinked:
    e.reset();

    // Assert that the callback did not log a warning:
    CHECK(warningsLogged() == 0);
}
#endif


TEST_CASE_METHOD(QueryTest, "Query deleted docs", "[Query]") {
    addNumberedDocs(1, 10);
    {
        Transaction t(store->dataFile());
        for (int i = 11; i <= 20; i++)
            writeNumberedDoc(i, nullslice, t,
                             DocumentFlags::kDeleted | DocumentFlags::kHasAttachments);
        t.commit();
    }

    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['<=', ['.num'], 15]}")) == 10);
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['AND', ['<=', ['.num'], 15], ['._deleted']]}")) == 5);
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['OR',['=',['._deleted'],false],['=',['._deleted'],true]]}")) == 20);
}


TEST_CASE_METHOD(QueryTest, "Query expiration", "[Query]") {
    addNumberedDocs(1, 3);
    expiration_t now = KeyStore::now();

    {
        Retained<Query> query{ store->compileQuery(json5(
            "{WHAT: ['._id'], WHERE: ['.expiration']}")) };
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
        CHECK(!e->next());
    }

    store->setExpiration("rec-001"_sl, now - 10000);
    store->setExpiration("rec-003"_sl, now + 10000);

    {
        Retained<Query> query{ store->compileQuery(json5(
            "{WHAT: ['._expiration'], ORDER_BY: [['._id']]}")) };
        unique_ptr<QueryEnumerator> e(query->createEnumerator());
        CHECK(e->next());
        CHECK(e->columns()[0]->asInt() == now - 10000);
        CHECK(e->next());
        CHECK(e->columns()[0]->type() == kNull);
        CHECK(e->missingColumns() == 1);
        CHECK(e->next());
        CHECK(e->columns()[0]->asInt() == now + 10000);
        CHECK(!e->next());
    }
    {
        Retained<Query> query{ store->compileQuery(json5(
            "{WHAT: ['._id'], WHERE: ['<=', ['._expiration'], ['$NOW']], ORDER_BY: [['._expiration']]}")) };

        Query::Options options { alloc_slice(format("{\"NOW\": %lld}", (long long)now)) };
        
        unique_ptr<QueryEnumerator> e(query->createEnumerator(&options));
        CHECK(e->next());
        CHECK(e->columns()[0]->asString() == "rec-001"_sl);
        CHECK(!e->next());
    }
}


TEST_CASE_METHOD(QueryTest, "Query Dictionary Literal", "[Query]") {
    {
        Transaction t(store->dataFile());
        string docID = "rec-00";
        
        stringstream ss(docID);
        writeDoc(slice(ss.str()), DocumentFlags::kNone, t, [=](Encoder &enc) {
            enc.writeKey("string");
            enc.writeString("string");
            enc.writeKey("int_min");
            enc.writeInt(INT64_MIN);
            enc.writeKey("int_max");
            enc.writeInt(INT64_MAX);
            enc.writeKey("flt_min");
            enc.writeDouble(FLT_MIN);
            enc.writeKey("flt_max");
            enc.writeDouble(FLT_MAX);
            enc.writeKey("dbl_min");
            enc.writeDouble(DBL_MIN);
            enc.writeKey("dbl_max");
            enc.writeDouble(DBL_MAX);
            enc.writeKey("bool_true");
            enc.writeBool(true);
            enc.writeKey("bool_false");
            enc.writeBool(false);
        });
        t.commit();
    }
    
    auto query = store->compileQuery(json5(
       "{'WHAT': [{ \
           string: ['.string'], \
           int_min: ['.int_min'], \
           int_max: ['.int_max'], \
           flt_min: ['.flt_min'], \
           flt_max: ['.flt_max'], \
           dbl_min: ['.dbl_min'], \
           dbl_max: ['.dbl_max'], \
           bool_true: ['.bool_true'], \
           bool_false: ['.bool_false'] \
        }]}"));
    
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asDict());
    CHECK(e->columns()[0]->asDict()->get("string"_sl)->asString() == "string"_sl);
    CHECK(e->columns()[0]->asDict()->get("int_min"_sl)->asInt() == INT64_MIN);
    CHECK(e->columns()[0]->asDict()->get("int_max"_sl)->asInt() == INT64_MAX);
    CHECK(e->columns()[0]->asDict()->get("flt_min"_sl)->asFloat() == FLT_MIN);
    CHECK(e->columns()[0]->asDict()->get("flt_max"_sl)->asFloat() == FLT_MAX);
    CHECK(e->columns()[0]->asDict()->get("dbl_min"_sl)->asDouble() == DBL_MIN);
    CHECK(e->columns()[0]->asDict()->get("dbl_max"_sl)->asDouble() == DBL_MAX);
    CHECK(e->columns()[0]->asDict()->get("bool_true"_sl)->asBool() == true);
    CHECK(e->columns()[0]->asDict()->get("bool_false"_sl)->asBool() == false);
}


TEST_CASE_METHOD(QueryTest, "Query N1QL", "[Query]") {
    addNumberedDocs();
    Retained<Query> query{ store->compileQuery("SELECT num, num*num WHERE num >= 30 and num <= 40 ORDER BY num"_sl,
                                               QueryLanguage::kN1QL) };
    CHECK(query->columnCount() == 2);
    int num = 30;
    unique_ptr<QueryEnumerator> e(query->createEnumerator());
    while (e->next()) {
        string expectedDocID = stringWithFormat("rec-%03d", num);
        auto cols = e->columns();
        REQUIRE(cols.count() == 2);
        REQUIRE(cols[0]->asInt() == num);
        REQUIRE(cols[1]->asInt() == num * num);
        ++num;
    }
    REQUIRE(num == 41);
}
