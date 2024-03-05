//
// QueryTest.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "QueryTest.hh"
#include "SQLiteDataFile.hh"
#include "Benchmark.hh"
#include <cstdint>
#include <ctime>
#include <cfloat>
#include <cinttypes>
#include <chrono>
#include <limits>
#include <numeric>
#include "date/date.h"
#include "ParseDate.hh"
#include <functional>

using namespace fleece::impl;
using namespace std;
using namespace std::chrono;
using namespace date;

unsigned QueryTest::alter2 = 0;
unsigned QueryTest::alter3 = 0;

N_WAY_TEST_CASE_METHOD(QueryTest, "Create/Delete Index", "[Query][FTS]") {
    addArrayDocs();

    IndexSpec::FTSOptions options{"en", true};
    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter,
                    [=] { store->createIndex(""_sl, R"([[".num"]])"); });

    ExpectException(error::Domain::LiteCore, error::LiteCoreError::InvalidParameter,
                    [=] { store->createIndex(R"("num")", R"([[".num"]])", IndexSpec::kFullText, options); });

    auto allKeyStores = db->allKeyStoreNames();

    CHECK(store->createIndex("num"_sl, "[[\".num\"]]"_sl, IndexSpec::kValue, options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num"}));
    CHECK(!store->createIndex("num"_sl, "[[\".num\"]]"_sl, IndexSpec::kValue, options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num"}));

    CHECK(store->createIndex("num"_sl, "[[\".num\"]]"_sl, IndexSpec::kFullText, options));
    CHECK(!store->createIndex("num"_sl, "[[\".num\"]]"_sl, IndexSpec::kFullText, options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num"}));
    CHECK(db->allKeyStoreNames() == allKeyStores);  // CBL-3824, CBL-5369

    store->deleteIndex("num"_sl);
    CHECK(store->createIndex("num_second"_sl, "[[\".num\"]]"_sl, IndexSpec::kFullText, options));
    CHECK(store->createIndex("num_second"_sl, "[[\".num_second\"]]"_sl, IndexSpec::kFullText, options));
    CHECK(extractIndexes(store->getIndexes()) == vector<string>{"num_second"});

    CHECK(store->createIndex("num"_sl, "[\".num\"]"_sl));
    CHECK(store->createIndex("num_second"_sl, "[\".num\"]"_sl));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num", "num_second"}));
    store->deleteIndex("num"_sl);
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"num_second"}));

    CHECK(store->createIndex("array_1st"_sl, "[[\".numbers\"]]"_sl, IndexSpec::kArray, options));
    CHECK(!store->createIndex("array_1st"_sl, "[[\".numbers\"]]"_sl, IndexSpec::kArray, options));
    CHECK(store->createIndex("array_2nd"_sl, "[[\".numbers\"],[\".key\"]]"_sl, IndexSpec::kArray, options));
    CHECK(extractIndexes(store->getIndexes()) == (vector<string>{"array_1st", "array_2nd", "num_second"}));
    CHECK(db->allKeyStoreNames() == allKeyStores);  // CBL-3824, CBL-5369

    store->deleteIndex("num_second"_sl);
    store->deleteIndex("num_second"_sl);  // Duplicate should be no-op
    store->deleteIndex("array_1st"_sl);
    store->deleteIndex("array_1st"_sl);  // Duplicate should be no-op
    store->deleteIndex("array_2nd"_sl);
    store->deleteIndex("array_2nd"_sl);  // Duplicate should be no-op
    CHECK(extractIndexes(store->getIndexes()).empty());
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Create/Delete Array Index", "[Query][ArrayIndex]") {
    addArrayDocs();
    store->createIndex("nums"_sl, R"([[".numbers"]])", IndexSpec::kArray);
    store->deleteIndex("nums"_sl);
}

TEST_CASE_METHOD(QueryTest, "Create Partial Index", "[Query]") {
    addNumberedDocs(1, 100);
    addArrayDocs(101, 100);

    store->createIndex("nums"_sl, R"({"WHAT":[[".num"]], "WHERE":["=",[".type"],"number"]})"_sl);

    auto [queryJson, expectOptimized] =
            GENERATE(pair<const char*, bool>{"['AND', ['=', ['.type'], 'number'], "
                                             "['>=', ['.', 'num'], 30], ['<=', ['.', 'num'], 40]]",
                                             true},
                     pair<const char*, bool>{"['AND', ['>=', ['.', 'num'], 30], ['<=', ['.', 'num'], 40]]", false});
    logSection(string("Query: ") + queryJson);
    Retained<Query> query = store->compileQuery(json5(queryJson));
    checkOptimized(query, expectOptimized);

    int64_t     rowCount;
    alloc_slice rows;
    ((SQLiteDataFile&)store->dataFile()).inspectIndex("nums"_sl, rowCount, &rows);
    string rowsJSON = Value::fromTrustedData(rows)->toJSONString();
    Log("Index has %" PRIi64 " rows", rowCount);
    Log("Index contents: %s", rowsJSON.c_str());
    CHECK(rowCount == 100);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Partial Index with NOT MISSING", "[Query]") {
    // This tests whether SQLite is smart enough to know that a query on a property (.num) can
    // use a partial index whose condition is that the property is not MISSING.
    // Apparently it is :)
    addNumberedDocs(1, 100);
    addArrayDocs(101, 100);

    store->createIndex("nums"_sl, R"({"WHAT":[[".num"]], "WHERE":["IS NOT",[".num"],["MISSING"]]})"_sl);

    const char* queryJson = "['AND', ['>=', ['.', 'num'], 30], ['<=', ['.', 'num'], 40]]";

    Retained<Query> query = store->compileQuery(json5(queryJson));
    checkOptimized(query);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query SELECT", "[Query]") {
    addNumberedDocs();
    // Use a (SQL) query based on the Fleece "num" property:
    Retained<Query> query{store->compileQuery(json5("['AND', ['>=', ['.', 'num'], 30], ['<=', ['.', 'num'], 40]]"))};
    CHECK(query->columnCount() == 2);  // docID and sequence, by default

    for ( int pass = 0; pass < 2; ++pass ) {
        Stopwatch                 st;
        int                       i = 30;
        Retained<QueryEnumerator> e(query->createEnumerator());
        while ( e->next() ) {
            auto cols = e->columns();
            REQUIRE(e->columns().count() == 2);
            slice  docID         = cols[0]->asString();
            auto   seq           = sequence_t(cols[1]->asInt());
            string expectedDocID = stringWithFormat("rec-%03d", i);
            REQUIRE(docID == slice(expectedDocID));
            REQUIRE(seq == (sequence_t)i);
            ++i;
        }
        st.printReport("Query of $.num", i, "row");
        REQUIRE(i == 41);

        // Add an index after the first pass:
        if ( pass == 0 ) {
            Stopwatch st2;
            store->createIndex("num"_sl, R"([".num"])");
            st2.printReport("Index on .num", 1, "index");
        }
    }

    // Redundant createIndex should not fail:
    store->createIndex("num"_sl, R"([".num"])");
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query SELECT WHAT", "[Query][N1QL]") {
    addNumberedDocs();
    auto language = GENERATE(QueryLanguage::kJSON, QueryLanguage::kN1QL);
    auto str =
            language == QueryLanguage::kJSON
                    ? json5("{WHAT: ['.num', ['AS', ['*', ['.num'], ['.num']], 'square']], WHERE: ['>', ['.num'], 10]}")
                    : string("SELECT num, num*num AS square FROM ") + collectionName + " WHERE num > 10";
    logSection(str);
    Retained<Query> query = store->compileQuery(str, language);
    CHECK(query->columnCount() == 2);
    CHECK(query->columnTitles() == (vector<string>{"num", "square"}));
    int                       num = 11;
    Retained<QueryEnumerator> e(query->createEnumerator());
    while ( e->next() ) {
        string expectedDocID = stringWithFormat("rec-%03d", num);
        auto   cols          = e->columns();
        REQUIRE(cols.count() == 2);
        REQUIRE(cols[0]->asInt() == num);
        REQUIRE(cols[1]->asInt() == num * num);
        ++num;
    }
    REQUIRE(num == 101);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query SELECT All", "[Query]") {
    addNumberedDocs();
    Retained<Query> query1{store->compileQuery(json5("{WHAT: [['.main'], ['*', ['.main.num'], ['.main.num']]], WHERE: "
                                                     "['>', ['.main.num'], 10], FROM: [{AS: 'main'}]}"))};
    Retained<Query> query2{store->compileQuery(json5("{WHAT: [ '.main',  ['*', ['.main.num'], ['.main.num']]], WHERE: "
                                                     "['>', ['.main.num'], 10], FROM: [{AS: 'main'}]}"))};

    CHECK(query1->columnTitles() == (vector<string>{"main", "$1"}));

    if ( GENERATE(false, true) ) {
        logSection("Ignore deleted docs");
        ExclusiveTransaction t(store->dataFile());
        for ( int i = 201; i <= 300; i++ )
            writeNumberedDoc(i, nullslice, t, DocumentFlags::kDeleted | DocumentFlags::kHasAttachments);
        t.commit();
    }

    int                       num = 11;
    Retained<QueryEnumerator> e(query1->createEnumerator());
    Retained<QueryEnumerator> e2(query1->createEnumerator());
    while ( e->next() && e2->next() ) {
        string expectedDocID = stringWithFormat("rec-%03d", num);
        auto   cols          = e->columns();
        auto   cols2         = e2->columns();
        REQUIRE(cols.count() == 2);
        REQUIRE(cols2.count() == 2);
        auto star  = cols[0]->asDict();
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

N_WAY_TEST_CASE_METHOD(QueryTest, "Query null value", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("null-and-void"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("n");
            enc.writeNull();
        });
        t.commit();
    }

    Retained<Query>           query{store->compileQuery(json5("{WHAT: [['.n'], ['.']]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
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

N_WAY_TEST_CASE_METHOD(QueryTest, "Query array_count null value", "[Query][CBL-3087]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("null-and-void"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("n");
            enc.beginArray();
            enc.writeNull();
            enc.writeString("abc");
            enc.writeString("def");
            enc.writeNull();
            enc.writeString("ghi");
            enc.endArray();
        });
        t.commit();
    }

    Retained<Query>           query{store->compileQuery(json5("{WHAT: [['ARRAY_COUNT()', ['.n']]]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    auto cols = e->columns();
    REQUIRE(cols.count() == 1);
    CHECK(cols[0]->type() == kNumber);
    CHECK(cols[0]->asInt() == 3);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query refresh", "[Query]") {
    addNumberedDocs();
    Retained<Query> query{
            store->compileQuery(json5("{WHAT: ['.num', ['*', ['.num'], ['.num']]], WHERE: ['>', ['.num'], 10]}"))};
    CHECK(query->columnCount() == 2);
    int                       num = 11;
    Retained<QueryEnumerator> e(query->createEnumerator());
    while ( e->next() ) ++num;
    REQUIRE(num == 101);

    CHECK(e->refresh(query) == nullptr);

    // Add a doc that doesn't alter the query:
    {
        ExclusiveTransaction t(db);
        writeNumberedDoc(-1, nullslice, t);
        t.commit();
    }
    CHECK(e->refresh(query) == nullptr);

#if 0  //FIX: This doesn't work yet, because the doc's sequence and revID are in the query results,                   \
        // and those do change...
    // Modify a doc in a way that doesn't affect the query results
    {
        Transaction t(db);
        writeNumberedDoc(20, "howdy"_sl, t);
        t.commit();
    }
    CHECK(e->refresh(query) == nullptr);
#endif

    // Delete one of the docs in the query -- this does trigger a refresh:
    deleteDoc("rec-030"_sl, false);

    Retained<QueryEnumerator> e2(e->refresh(query));
    REQUIRE(e2 != nullptr);

    num = 11;
    while ( e2->next() ) ++num;
    CHECK(num == 100);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query boolean", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        for ( int i = 0; i < 2; i++ ) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("value");
                enc.writeBool(i == 0);
            });
        }

        // Integer 0 and 1 would have fooled ISBOOLEAN() before
        for ( int i = 2; i < 4; i++ ) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("value");
                enc.writeInt(i - 2);
            });
        }

        t.commit();
    }

    // Check the data type of the returned values:
    Retained<Query>           query = store->compileQuery(json5("{WHAT: ['.value']}"));
    Retained<QueryEnumerator> e     = query->createEnumerator();
    REQUIRE(e->getRowCount() == 4);
    int row = 0;
    while ( e->next() ) {
        auto type = e->columns()[0]->type();
        if ( row < 2 ) CHECK(type == kBoolean);
        else
            CHECK(type == kNumber);
        ++row;
    }

    // Check the ISBOOLEAN function:
    query = store->compileQuery(json5("{WHAT: ['._id'], WHERE: ['ISBOOLEAN()', ['.value']]}"));
    CHECK(query->columnTitles() == (vector<string>{"id"}));
    e = query->createEnumerator();
    REQUIRE(e->getRowCount() == 2);
    int i = 1;
    while ( e->next() ) { CHECK(e->columns()[0]->asString().asString() == stringWithFormat("rec-%03d", i++)); }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query uint64", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        const int64_t        ivals[] = {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
        const uint64_t       uvals[] = {
                0,
                std::numeric_limits<uint64_t>::max(),
        };
        for ( int i = 0; i < 4; i++ ) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("value");
                if ( i < 2 ) enc.writeInt(ivals[i]);
                else
                    enc.writeUInt(uvals[i - 2]);
            });
        }
        t.commit();
    }

    // Check the data type of the returned values:
    Retained<Query>           query = store->compileQuery(json5("{WHAT: ['.value']}"));
    Retained<QueryEnumerator> e     = query->createEnumerator();
    REQUIRE(e->getRowCount() == 4);
    int row = 0;
    while ( e->next() ) {
        auto type = e->columns()[0]->type();
        CHECK(type == kNumber);
        auto isInt = e->columns()[0]->isInteger();
        CHECK(isInt);
        auto isUnsigned = e->columns()[0]->isUnsigned();
        if ( row < 3 ) CHECK(!isUnsigned);
        else
            CHECK(isUnsigned);
        ++row;
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query boolean return", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("tester"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeInt(1);
            enc.writeKey("col");
            enc.beginArray(4);
            enc.writeInt(1);
            enc.writeInt(2);
            enc.writeInt(3);
            enc.writeInt(4);
            enc.endArray();
        });
        t.commit();
    }

    string queries[] = {
            json5("{WHAT: [['>', ['.num'], 50]]}"),
            json5("{WHAT: [['<', ['.num'], 50]]}"),
            json5("{WHAT: [['>=', ['.num'], 50]]}"),
            json5("{WHAT: [['<=', ['.num'], 50]]}"),
            json5("{WHAT: [['=', ['.num'], 50]]}"),
            json5("{WHAT: [['!=', ['.num'], 50]]}"),
            json5("{WHAT: [['is', ['.num'], 50]]}"),
            json5("{WHAT: [['IS NOT', ['.num'], 50]]}"),
            json5("{WHAT: [['NOT', ['=', ['.num'], 50]]]}"),
            json5("{WHAT: [['IN', 3, ['.col']]]}"),
            json5("{WHAT: [['NOT IN', 3, ['.col']]]}"),
            json5("{WHAT: [['BETWEEN', ['.num'], 3, 5]]}"),
            json5("{WHAT: [['EXISTS', ['.num']]]}"),
            json5("{WHAT: [['AND', 3, 0]]}"),
            json5("{WHAT: [['OR', 3, 0]]}"),
            json5("{WHAT: [['ANY', 'num', ['.col'], ['=', ['?num'], 1]]]}"),
            json5("{WHAT: [['EVERY', 'num', ['.col'], ['>', ['?num'], 0]]]}"),
            json5("{WHAT: [['ANY AND EVERY', 'num', ['.col'], ['>', ['?num'], 0]]]}"),
            json5("{WHAT: [['CONTAINS()', ['.col'], 1]]}"),
            json5("{WHAT: [['FL_LIKE()', ['.num'], '1%']]}"),
            json5("{WHAT: [['REGEXP_LIKE()', ['TOSTRING()', ['.num']], '1\\\\d+']]}"),
            json5("{WHAT: [['ISARRAY()', ['.num']]]}"),
            json5("{WHAT: [['ISARRAY()', ['.num']]]}"),
            json5("{WHAT: [['ISATOM()', ['.num']]]}"),
            json5("{WHAT: [['ISBOOLEAN()', ['.num']]]}"),
            json5("{WHAT: [['ISNUMBER()', ['.num']]]}"),
            json5("{WHAT: [['ISOBJECT()', ['.num']]]}"),
            json5("{WHAT: [['ISSTRING()', ['.num']]]}"),
            json5("{WHAT: [['TOBOOLEAN()', ['.num']]]}"),
    };

    for ( const auto& query : queries ) {
        Retained<Query>           q = store->compileQuery(query);
        Retained<QueryEnumerator> e = q->createEnumerator();
        REQUIRE(e->getRowCount() == 1);
        REQUIRE(e->next());
        CHECK(e->columns()[0]->type() == kBoolean);
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query weird property names", "[Query]") {
    // For <https://github.com/couchbase/couchbase-lite-core/issues/545>
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("$foo");
            enc.writeInt(17);
            enc.writeKey("?foo");
            enc.writeInt(18);
            enc.writeKey("[foo");
            enc.writeInt(19);
            enc.writeKey(".foo");
            enc.writeInt(20);
            enc.writeKey("f$o?o[o.");
            enc.writeInt(21);
            enc.writeKey("oids");
            enc.beginArray();
            enc.beginDictionary();
            enc.writeKey("$oid");
            enc.writeString("avoid");
            enc.endDictionary();
            enc.endArray();
            enc.writeKey("str]ing]");
            enc.writeInt(10);
            enc.writeKey("string[1]");
            enc.beginDictionary();
            enc.writeKey("arr");
            enc.beginArray();
            enc.beginDictionary();
            enc.writeKey("string[3]");
            enc.beginDictionary();
            enc.writeKey("simpleID");
            enc.writeInt(20);
            enc.endDictionary();
            enc.endDictionary();
            enc.endArray();
            enc.endDictionary();
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

    Retained<Query>           query;
    Retained<QueryEnumerator> e;
    if ( option == 0 ) {
        query = store->compileQuery("SELECT `str]ing]` FROM _"_sl, QueryLanguage::kN1QL);
    } else {
        query = store->compileQuery(json5("{WHAT: ['.str]ing]']}"), QueryLanguage::kJSON);
    }
    REQUIRE(query);
    e = query->createEnumerator();
    CHECK(e->columns().count() == 1);
    CHECK(e->columns()[0]->asInt() == 10);
    if ( option == 0 ) {
        query = store->compileQuery("SELECT `string[1]`.arr[0].`string[3]`.simpleID FROM _"_sl, QueryLanguage::kN1QL);
    } else {
        query = store->compileQuery(json5("{WHAT: ['.string\\\\[1].arr[0].string\\\\[3].simpleID']}"),
                                    QueryLanguage::kJSON);
    }
    e = query->createEnumerator();
    CHECK(e->columns().count() == 1);
    CHECK(e->columns()[0]->asInt() == 20);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query object properties", "[Query]") {
    {
        ExclusiveTransaction t(db);
        writeMultipleTypeDocs(t);
        t.commit();
    }

    Retained<Query> query{store->compileQuery(json5("['=', 'FTW', ['_.subvalue', ['.value']]]"))};

    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "doc4"_sl);
    CHECK(!e->next());
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query array index", "[Query]") {
    addArrayDocs();

    Retained<Query> query{store->compileQuery(json5("['=', 'five', ['_.[0]', ['.numbers']]]"))};

    Retained<QueryEnumerator> e(query->createEnumerator());
    int                       i = 0;
    while ( e->next() ) {
        slice docID = e->columns()[0]->asString();
        CHECK(docID == "rec-010"_sl);
        ++i;
    }
    REQUIRE(i == 1);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query array literal", "[Query]") {
    addNumberedDocs(1, 1);
    Retained<Query> query{
            store->compileQuery(json5("{WHAT: [['[]', null, false, true, 12345, 1234.5, 'howdy', ['._id']]]}"))};

    CHECK(query->columnTitles() == (vector<string>{"$1"}));

    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSONString() == "[null,false,true,12345,1234.5,\"howdy\",\"rec-001\"]");
    CHECK(!e->next());
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query dict literal", "[Query]") {
    addNumberedDocs(1, 1);
    Retained<Query> query{store->compileQuery(json5(
            "{WHAT: [{n: null, f: false, t: true, i: 12345, d: 1234.5, s: 'howdy', m: ['.bogus'], id: ['._id']}]}"))};

    CHECK(query->columnTitles() == (vector<string>{"$1"}));

    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSON<5>() == "{d:1234.5,f:false,i:12345,id:\"rec-001\",n:null,s:\"howdy\",t:true}"_sl);
    CHECK(!e->next());
}

TEST_CASE_METHOD(QueryTest, "Column Title of unqualified star", "[Query]") {
    addNumberedDocs(1, 1);
    string           sqls[][2] = {{"SELECT *         FROM                 _default._default", "_default"},
                                  {"SELECT *         FROM                          _default", "_default"},
                                  {"SELECT *         FROM                                 _", "_"},
                                  {"SELECT * AS Star FROM                 _default._default", "Star"},
                                  {"SELECT *         FROM _default._default AS MyCollection", "MyCollection"},
                                  {"SELECT * Star    FROM _default._default AS MyCollection", "Star"}};
    constexpr size_t sqlCount  = sizeof(sqls) / sizeof(string) / 2;
    Retained<Query>  query;
    for ( size_t i = 0; i < sqlCount; ++i ) {
        query = store->compileQuery(sqls[i][0], litecore::QueryLanguage::kN1QL);
        CHECK(query->columnTitles()[0] == sqls[i][1]);
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query dict literal with blob", "[Query]") {
    // Create a doc with a blob property:
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("goop"_sl, (DocumentFlags)0, t, [](Encoder& enc) {
            enc.writeKey("text");
            enc.beginDictionary();
            enc.writeKey("@type");
            enc.writeString("blob");
            enc.writeKey("digest");
            enc.writeString("xxxxxxx");
            enc.endDictionary();
        });
        t.commit();
    }

    Retained<Query> query{store->compileQuery(json5("{WHAT:[ ['.text'], {'text':['.text']} ]}"))};
    Log("%s", query->explain().c_str());
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    auto d = e->columns()[0]->asDict();
    REQUIRE(d);
    CHECK(d->toJSON(true) == "{\"@type\":\"blob\",\"digest\":\"xxxxxxx\"}"_sl);
    REQUIRE(!e->next());
}

#pragma mark Targeted N1QL-function tests

N_WAY_TEST_CASE_METHOD(QueryTest, "Query array length", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        for ( int i = 0; i < 2; i++ ) {
            string docID = stringWithFormat("rec-%03d", i + 1);
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("value");
                enc.beginArray(i + 1);
                for ( int j = 0; j < i + 1; j++ ) { enc.writeInt(j); }
                enc.endArray();
            });
        }

        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{WHAT: ['._id'], WHERE: ['>', ['ARRAY_LENGTH()', ['.value']], 1]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "rec-002"_sl);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query missing and null", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        string               docID = "doc1";
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeNull();
            enc.writeKey("real_value");
            enc.writeInt(1);
        });
        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeNull();
            enc.writeKey("atai");
            enc.writeInt(1);
        });
        t.commit();
    }

    Retained<Query>           query{store->compileQuery(
            json5("{'WHAT': ['._id'], WHERE: ['=', ['IFMISSING()', ['.bogus'], ['MISSING'], ['.value']], null]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery(
            json5("{'WHAT': ['._id'], WHERE: ['=', ['IFMISSINGORNULL()', ['.atai'], ['.value']], 1]}"));
    e = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery(
            json5("{'WHAT': ['._id'], WHERE: ['=', ['IFMISSINGORNULL()', ['.atai'], ['.value']], null]}"));
    e = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);

    query = store->compileQuery(
            json5("{'WHAT': ['._id'], WHERE: ['=', ['IFNULL()', ['.value'], ['.real_value'], ['.atai']], 1]}"));
    e = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);

    query = store->compileQuery(json5(
            "{'WHAT': ['._id'], WHERE: ['=', ['IFMISSINGORNULL()', ['.real_value'], ['.value'], ['.atai']], 1]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery(json5("{'WHAT': ['._id'], WHERE: ['=', ['MISSINGIF()', ['.real_value'], 3], 1]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);

    query = store->compileQuery(
            json5("{'WHAT': ['._id'], WHERE: ['IS', ['MISSINGIF()', ['.real_value'], 1], ['MISSING']]}"));
    e = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);

    query = store->compileQuery(
            json5("{'WHAT': ['._id'], WHERE: ['IS', ['MISSINGIF()', ['.value'], 1], ['MISSING']]}"));
    e = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 0);

    query = store->compileQuery(json5("{'WHAT': ['._id'], WHERE: ['IS', ['MISSINGIF()', ['.value'], 1], null]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);

    query = store->compileQuery(json5("{'WHAT': ['._id'], WHERE: ['=', ['NULLIF()', 3, ['.atai']], 3]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery(json5("{'WHAT': ['._id'], WHERE: ['=', ['NULLIF()', 1, ['.atai']], null]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery(json5("{'WHAT': ['._id'], WHERE: ['IS', ['NULLIF()', 1, ['.atai']], ['MISSING']]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query concat", "[Query]") {
    addNumberedDocs(1, 1);
    CHECK(queryWhat("['concat()', 'hello', 'world']") == "\"helloworld\"");
    CHECK(queryWhat("['||', 'hello', 'world']") == "\"helloworld\"");
    CHECK(queryWhat("['concat()', 'hello', ' ', 'world']") == "\"hello world\"");
    CHECK(queryWhat("['concat()', 99, ' ', 123.45, ' ', true, ' ', false]") == "\"99 123.45 true false\"");

    CHECK(queryWhat("['concat()', 'goodbye ', null, ' world']") == "\"goodbye null world\"");
    CHECK(queryWhat("['concat()', 'goodbye', ['.bogus'], 'world']") == "null");
    CHECK(queryWhat("['concat()', 'goodbye', ' ', ['.num'], ' ', ['.type']]") == "\"goodbye 1 number\"");
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query regex", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeString("awesome value");
        });
        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeString("cool value");
        });
        writeDoc("doc3"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeString("invalid");
        });
        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': ['._id'], WHERE: ['REGEXP_CONTAINS()', ['.value'], '.*? value']}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery(json5("{'WHAT': ['._id'], WHERE: ['REGEXP_LIKE()', ['.value'], '.*? value']}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery(
            json5("{'WHAT': ['._id'], WHERE: ['>', ['REGEXP_POSITION()', ['.value'], '[ ]+value'], 4]}"));
    e = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "doc1"_sl);

    query = store->compileQuery(json5("{'WHAT': [['REGEXP_REPLACE()', ['.value'], '.*?value', 'nothing']]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 3);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "nothing"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "nothing"_sl);
    REQUIRE(e->next());
    REQUIRE(e->columns()[0]->asString() == "invalid"_sl);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query type check", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': [['TYPE()', ['.value']], ['._id']], WHERE: "
                                      "['AND', ['ISARRAY()', ['.value']], ['IS_ARRAY()', ['.value']]]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "array"_sl);
    CHECK(e->columns()[1]->asString() == "doc1"_sl);

    query = store->compileQuery(json5("{'WHAT': [['TYPENAME()', ['.value']], ['._id'], ['.value']], WHERE: "
                                      "['AND', ['ISNUMBER()', ['.value']], ['IS_NUMBER()', ['.value']]]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "number"_sl);
    CHECK(e->columns()[1]->asString() == "doc3"_sl);
    CHECK(e->columns()[2]->asDouble() == 4.5);

    query = store->compileQuery(json5("{'WHAT': [['TYPE()', ['.value']], ['._id'], ['.value']], WHERE: "
                                      "['AND', ['ISSTRING()', ['.value']], ['IS_STRING()', ['.value']]]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "string"_sl);
    CHECK(e->columns()[1]->asString() == "doc2"_sl);
    CHECK(e->columns()[2]->asString() == "cool value"_sl);

    query = store->compileQuery(json5("{'WHAT': [['TYPENAME()', ['.value']], ['._id']], WHERE: "
                                      "['AND', ['ISOBJECT()', ['.value']], ['IS_OBJECT()', ['.value']]]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "object"_sl);
    CHECK(e->columns()[1]->asString() == "doc4"_sl);

    query = store->compileQuery(json5("{'WHAT': [['TYPE()', ['.value']], ['._id'], ['.value']], WHERE: "
                                      "['AND', ['ISBOOLEAN()', ['.value']], ['IS_BOOLEAN()', ['.value']]]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "boolean"_sl);
    CHECK(e->columns()[1]->asString() == "doc5"_sl);
    CHECK(e->columns()[2]->asBool());

    query = store->compileQuery(json5("{'WHAT': [['TYPENAME()', ['.value']], ['._id']], WHERE: "
                                      "['AND', ['ISATOM()', ['.value']], ['IS_ATOM()', ['.value']]]}"));
    e     = (query->createEnumerator());
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

N_WAY_TEST_CASE_METHOD(QueryTest, "Query toboolean", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        writeFalselyDocs(t);
        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': [['TOBOOLEAN()', ['.value']], ['TO_BOOLEAN()', ['.value']]]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 8);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    CHECK(e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    CHECK(e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    CHECK(e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    CHECK(e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    CHECK(e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
    CHECK(!e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
    CHECK(!e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
    CHECK(!e->columns()[1]->asBool());
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query toatom", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        writeFalselyDocs(t);
        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': [['TOATOM()', ['.value']], ['TO_ATOM()', ['.value']]]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 8);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
    CHECK(e->columns()[1]->asInt() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "cool value"_sl);
    CHECK(e->columns()[1]->asString() == "cool value"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 4.5);
    CHECK(e->columns()[1]->asDouble() == 4.5);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "FTW"_sl);
    CHECK(e->columns()[1]->asString() == "FTW"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asBool());
    CHECK(e->columns()[1]->asBool());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    REQUIRE(e->next());
    CHECK(!e->columns()[0]->asBool());
    CHECK(!e->columns()[1]->asBool());
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query tonumber", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        writeDoc("doc6"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeString("602214076000000000000000");  // overflows uint64_t
        });
        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': [['TONUMBER()', ['.value']], ['TO_NUMBER()', ['.value']]]}"))};
    Retained<QueryEnumerator> e;
    {
        ExpectingExceptions x;  // tonumber() will internally throw/catch an exception while indexing
        e = (query->createEnumerator());
    }
    REQUIRE(e->getRowCount() == 6);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 0.0);
    CHECK(e->columns()[1]->asDouble() == 0.0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 0.0);
    CHECK(e->columns()[1]->asDouble() == 0.0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 4.5);
    CHECK(e->columns()[1]->asDouble() == 4.5);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 0.0);
    CHECK(e->columns()[1]->asDouble() == 0.0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 1.0);
    CHECK(e->columns()[1]->asDouble() == 1.0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == 6.02214076e23);
    CHECK(e->columns()[1]->asDouble() == 6.02214076e23);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query tostring", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeMultipleTypeDocs(t);
        writeFalselyDocs(t);
        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': [['TOSTRING()', ['.value']], ['TO_STRING()', ['.value']]]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 8);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "cool value"_sl);
    CHECK(e->columns()[1]->asString() == "cool value"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString().asString().substr(0, 3) == "4.5");
    CHECK(e->columns()[1]->asString().asString().substr(0, 3) == "4.5");
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "true"_sl);
    CHECK(e->columns()[1]->asString() == "true"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "false"_sl);
    CHECK(e->columns()[1]->asString() == "false"_sl);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query toarray", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());

        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            // [1]
            enc.beginArray();
            enc.writeInt(1);
            enc.endArray();
        });
        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeString("string value");
        });
        writeDoc("doc3"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeDouble(4.5);
        });
        writeDoc("doc4"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            // { "subvalue": "FTW" }
            enc.beginDictionary(1);
            enc.writeKey("subvalue");
            enc.writeString("FTW");
            enc.endDictionary();
        });
        writeDoc("doc5"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeNull();
        });

        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': [['TOARRAY()', ['.value']], ['TO_ARRAY()', ['.value']]]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 5);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kArray);
    CHECK(e->columns()[0]->asArray()->count() == 1);
    CHECK(e->columns()[1]->type() == kArray);
    CHECK(e->columns()[1]->asArray()->count() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kArray);
    CHECK(e->columns()[0]->asArray()->get(0)->asString() == "string value"_sl);
    CHECK(e->columns()[1]->type() == kArray);
    CHECK(e->columns()[1]->asArray()->get(0)->asString() == "string value"_sl);
    REQUIRE(e->next());
    CHECK(e->missingColumns() == uint64_t(3));
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kArray);
    CHECK(e->columns()[0]->asArray()->get(0)->asDict()->get("subvalue"_sl)->asString() == "FTW"_sl);
    CHECK(e->columns()[1]->type() == kArray);
    CHECK(e->columns()[1]->asArray()->get(0)->asDict()->get("subvalue"_sl)->asString() == "FTW"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    CHECK(e->missingColumns() == uint64_t(0));
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query toobject", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());

        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            // [1]
            enc.beginArray();
            enc.writeInt(1);
            enc.endArray();
        });
        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeString("string value");
        });
        writeDoc("doc3"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeDouble(4.5);
        });
        writeDoc("doc4"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            // { "subvalue": "FTW" }
            enc.beginDictionary(1);
            enc.writeKey("subvalue");
            enc.writeString("FTW");
            enc.endDictionary();
        });
        writeDoc("doc5"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeNull();
        });

        t.commit();
    }

    Retained<Query> query{
            store->compileQuery(json5("{'WHAT': [['TOOBJECT()', ['.value']], ['TO_OBJECT()', ['.value']]]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 5);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kDict);
    CHECK(e->columns()[0]->asDict()->count() == 0);
    CHECK(e->columns()[1]->type() == kDict);
    CHECK(e->columns()[1]->asDict()->count() == 0);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kDict);
    CHECK(e->columns()[0]->asDict()->count() == 0);
    CHECK(e->columns()[1]->type() == kDict);
    CHECK(e->columns()[1]->asDict()->count() == 0);
    REQUIRE(e->next());
    CHECK(e->missingColumns() == uint64_t(3));
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kDict);
    CHECK(e->columns()[0]->asDict()->get("subvalue"_sl)->asString() == "FTW"_sl);
    CHECK(e->columns()[1]->type() == kDict);
    CHECK(e->columns()[1]->asDict()->get("subvalue"_sl)->asString() == "FTW"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->type() == kNull);
    CHECK(e->columns()[1]->type() == kNull);
    CHECK(e->missingColumns() == uint64_t(0));
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query HAVING", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());

        constexpr size_t bufSize = 6;
        char             docID[bufSize];
        for ( int i = 0; i < 20; i++ ) {
            snprintf(docID, bufSize, "doc%02d", i);
            writeDoc(slice(docID), DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("identifier");
                enc.writeInt(i >= 5 ? i >= 15 ? 3 : 2 : 1);
                enc.writeKey("index");
                enc.writeInt(i);
            });
        }

        t.commit();
    }


    Retained<Query> query{store->compileQuery(json5("{'WHAT': ['.identifier', ['COUNT()', ['.index']]], 'GROUP_BY': "
                                                    "['.identifier'], 'HAVING': ['=', ['.identifier'], 1]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
    CHECK(e->columns()[1]->asInt() == 5);

    query = store->compileQuery(json5("{'WHAT': ['.identifier', ['COUNT()', ['.index']]], 'GROUP_BY': ['.identifier'], "
                                      "'HAVING': ['!=', ['.identifier'], 1]}"));
    e     = (query->createEnumerator());

    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 2);
    CHECK(e->columns()[1]->asInt() == 10);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 3);
    CHECK(e->columns()[1]->asInt() == 5);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query Functions", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeNumberedDoc(1, nullslice, t);

        t.commit();
    }

    auto                      query = store->compileQuery(json5("{'WHAT': [['e()']]}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == M_E);

    query = store->compileQuery(json5("{'WHAT': [['pi()']]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asDouble() == M_PI);

    vector<string> what{"[['atan2()', ['.num'], ['*', ['.num'], 2]]]",
                        "[['acos()', ['.num']]]",
                        "[['asin()', ['.num']]]",
                        "[['atan()', ['.num']]]",
                        "[['cos()', ['.num']]]",
                        "[['degrees()', ['.num']]]",
                        "[['radians()', ['.num']]]",
                        "[['sin()', ['.num']]]",
                        "[['tan()', ['.num']]]"};

    vector<double> results{
            atan2(1, 2), acos(1), asin(1), atan(1), cos(1), 180.0 / M_PI, M_PI / 180.0, sin(1), tan(1),
    };

    for ( int i = 0; i < what.size(); i++ ) {
        query = store->compileQuery(json5("{'WHAT': " + what[i] + "}"));
        e     = (query->createEnumerator());
        REQUIRE(e->getRowCount() == 1);
        REQUIRE(e->next());
        CHECK(e->columns()[0]->asDouble() == results[i]);
    }

    query = store->compileQuery(json5("{'WHAT': [['sign()', ['.num']]]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
}


#ifdef COUCHBASE_ENTERPRISE
N_WAY_TEST_CASE_METHOD(QueryTest, "Query Distance Metrics", "[Query]") {
    testExpressions({
            {"['euclidean_distance()', ['[]', 10, 10], ['[]', 13, 14]]", "5.0"},
            {"['euclidean_distance()', ['[]', 10, 10], ['[]', 13, 14], 2]", "25.0"},
            {"['euclidean_distance()', ['[]', 1,2,3,4,5], ['[]', 1,2,3,4,5]]", "0.0"},
            {"['euclidean_distance()', ['[]'], ['[]']]", "0.0"},
            {"['euclidean_distance()', 18, 'foo']", "null"},
            {"['euclidean_distance()', ['[]', 10, 10], ['[]', 13]]", "null"},

            {"['cosine_distance()', ['[]', 10, 0], ['[]', 0, 99]]", "1.0"},
            {"['cosine_distance()', ['[]', 1,2,3,4,5], ['[]', 1,2,3,4,5]]", "0.0"},
            {"['cosine_distance()', ['[]'], ['[]']]", "null"},
            {"['cosine_distance()', 18, 'foo']", "null"},
            {"['cosine_distance()', ['[]', 10, 10], ['[]', 13]]", "null"},
    });
}
#endif


N_WAY_TEST_CASE_METHOD(QueryTest, "Query Date Functions", "[Query][CBL-59]") {
    constexpr local_seconds localtime      = local_days{2018_y / 10 / 23};
    tm                      tmpTime        = FromTimestamp(localtime.time_since_epoch());
    const seconds           offset_seconds = GetLocalTZOffset(&tmpTime, false);
    local_seconds           utc_time       = localtime - offset_seconds;

    // MILLIS_TO_STR() result should be in localtime.
    stringstream            mil_to_str;
    constexpr local_seconds mil_to_str_time = localtime + 18h + 33min + 1s;
    mil_to_str << date::format("%FT%T", mil_to_str_time + offset_seconds);
    if ( offset_seconds.count() == 0 ) {
        mil_to_str << "Z";
    } else {
        to_stream(mil_to_str, "%Ez", mil_to_str_time, nullptr, &offset_seconds);
    }
    const auto mil_to_str_expected = mil_to_str.str();

    // These are all for STR_TO_UTC
    stringstream s1, s2, s3, s5;
    stringstream s1iso, s2iso, s3iso;
    s1 << date::format("%F", utc_time);
    s1iso << date::format("%FT%TZ", utc_time);
    utc_time += 18h + 33min;
    s2 << date::format("%FT%T", utc_time);
    s2iso << date::format("%FT%TZ", utc_time);
    utc_time += 1s;
    s3 << date::format("%FT%T", utc_time);
    s3iso << date::format("%FT%TZ", utc_time);
    s5 << date::format("%FT%TZ", utc_time);

    constexpr local_seconds localtime2 = local_days{1944_y / 6 / 6} + 6h + 30min;
    tmpTime                            = FromTimestamp(localtime2.time_since_epoch());
    utc_time                           = localtime2 - GetLocalTZOffset(&tmpTime, false);
    stringstream s4;
    s4 << date::format("%FT%TZ", utc_time);

    auto expected1    = s1.str();
    auto expected2    = s2.str();
    auto expected3    = s3.str();
    auto expected4    = s4.str();
    auto expected5    = s5.str();
    auto expected1iso = s1iso.str();
    auto expected2iso = s2iso.str();
    auto expected3iso = s3iso.str();

    testExpressions({
            {"['str_to_utc()', null]", "null"},
            {"['str_to_utc()', 99]", "null"},
            {"['str_to_utc()', '']", "null"},
            {"['str_to_utc()', 'x']", "null"},
            {"['str_to_utc()', '2018-10-23', '1111-11-11']", expected1},
            {"['str_to_utc()', '2018-10-23']", expected1iso},
            {"['str_to_utc()', '2018-10-23T18:33', '1111-11-11T11:11']", expected2},
            {"['str_to_utc()', '2018-10-23T18:33']", expected2iso},
            {"['str_to_utc()', '2018-10-23T18:33:01', '1111-11-11T11:11:11']", expected3},
            {"['str_to_utc()', '2018-10-23T18:33:01']", expected3iso},
            {"['str_to_utc()', '1944-06-06T06:30:00']", expected4},
            {"['str_to_utc()', '2018-10-23T18:33:01Z']", "2018-10-23T18:33:01Z"},
            {"['str_to_utc()', '2018-10-23T11:33:01-0700']", "2018-10-23T18:33:01Z"},
            {"['str_to_utc()', '2018-10-23T11:33:01+03:30']", "2018-10-23T08:03:01Z"},
            {"['str_to_utc()', '2018-10-23T18:33:01.123Z']", "2018-10-23T18:33:01.123Z"},
            {"['str_to_utc()', '2018-10-23T11:33:01.123-0700']", "2018-10-23T18:33:01.123Z"},

            {"['str_to_millis()', '']", "null"},
            {"['str_to_millis()', '1970-01-01T00:00:00Z']", "0"},
            {"['str_to_millis()', '1944-06-06T06:30:00+01:00']", "-806956200000"},
            {"['str_to_millis()', '2018-10-23T11:33:01-0700']", "1540319581000"},
            {"['str_to_millis()', '2018-10-23T18:33:01Z']", "1540319581000"},
            {"['str_to_millis()', '2018-10-23T18:33:01.123Z']", "1540319581123"},

            // Range check the month and day number
            {"['str_to_millis()', '2000-00-01T00:00:00Z']", "null"},
            {"['str_to_millis()', '2000-13-01T00:00:00Z']", "null"},
            {"['str_to_millis()', '2000-01-00T00:00:00Z']", "null"},
            {"['str_to_millis()', '2000-01-32T00:00:00Z']", "null"},

            // 30 days hath September...
            {"['str_to_millis()', '2018-01-31T00:00:00Z']", "1517356800000"},
            {"['str_to_millis()', '2018-02-31T00:00:00Z']", "null"},
            {"['str_to_millis()', '2018-03-31T00:00:00Z']", "1522454400000"},
            {"['str_to_millis()', '2018-04-31T00:00:00Z']", "null"},
            {"['str_to_millis()', '2018-05-31T00:00:00Z']", "1527724800000"},
            {"['str_to_millis()', '2018-06-31T00:00:00Z']", "null"},
            {"['str_to_millis()', '2018-07-31T00:00:00Z']", "1532995200000"},
            {"['str_to_millis()', '2018-08-31T00:00:00Z']", "1535673600000"},
            {"['str_to_millis()', '2018-09-31T00:00:00Z']", "null"},
            {"['str_to_millis()', '2018-10-31T00:00:00Z']", "1540944000000"},
            {"['str_to_millis()', '2018-11-31T00:00:00Z']", "null"},
            {"['str_to_millis()', '2018-12-31T00:00:00Z']", "1546214400000"},

            // February is complicated
            {"['str_to_millis()', '2000-02-29T00:00:00Z']", "951782400000"},
            {"['str_to_millis()', '2016-02-29T00:00:00Z']", "1456704000000"},
            {"['str_to_millis()', '2018-02-29T00:00:00Z']", "null"},
            {"['str_to_millis()', '2100-02-29T00:00:00Z']", "null"},
            {"['str_to_millis()', '2400-02-29T00:00:00Z']", "13574563200000"},
            {"['str_to_millis()', '2400-02-30T00:00:00Z']", "null"},

            {"['millis_to_utc()', 'x']", "null"},
            {"['millis_to_utc()', '0']", "null"},
            {"['millis_to_utc()', 0]", "1970-01-01T00:00:00Z"},
            {"['millis_to_utc()', 1540319581000]", "2018-10-23T18:33:01Z"},
            {"['millis_to_utc()', 1540319581123]", "2018-10-23T18:33:01.123Z"},
            {"['millis_to_utc()', 1540319581999]", "2018-10-23T18:33:01.999Z"},
            {"['millis_to_utc()', -806956200000]", "1944-06-06T05:30:00Z"},
            {"['millis_to_utc()', 1540319581999, 'invalid']", "2018-10-23T18:33:01.999Z"},
            {"['millis_to_utc()', 1540319581999, '1111-11-11']", "2018-10-23"},
            {"['millis_to_utc()', 1540319581999, '11:11:11']", "18:33:01.999"},
            {"['millis_to_utc()', 1540319581999, '11:11:11Z']", "18:33:01.999Z"},
            {"['millis_to_utc()', 1540319581999, '11:11:11+09:00']", "18:33:01.999Z"},
            {"['millis_to_utc()', 1540319581999, '1111-11-11 11:11:11+09:00']", "2018-10-23 18:33:01.999Z"},
            {"['millis_to_utc()', 1540319581999, '1111-11-11T11:11:11+09:00']", "2018-10-23T18:33:01.999Z"},
            {"['millis_to_utc()', 1540319581999, '1111-11-11   T 11:11:11+09:00']", "2018-10-23T18:33:01.999Z"},

            {"['millis_to_str()', 1540319581000]", mil_to_str_expected},
            {"['str_to_utc()', ['millis_to_str()', 1540319581000]]", "2018-10-23T18:33:01Z"},
            {"['millis_to_str()', 'x']", "null"},
            {"['millis_to_str()', '0']", "null"},

            {"['str_to_tz()', '2024-01-10T14:31:14Z', 0]", "2024-01-10T14:31:14Z"},
            {"['str_to_tz()', '2024-01-10T14:31:14Z', -300]", "2024-01-10T09:31:14-05:00"},
            {"['str_to_tz()', '2024-01-10T14:31:14Z', +690]", "2024-01-11T02:01:14+11:30"},
            {"['millis_to_tz()', 1704897074000, 0]", "2024-01-10T14:31:14Z"},
            {"['millis_to_tz()', 1704897074000, -300]", "2024-01-10T09:31:14-05:00"},
            {"['millis_to_tz()', 1704897074000, +690]", "2024-01-11T02:01:14+11:30"},
            {"['millis_to_tz()', 1704897074000, +690, '1111-11-11']", "2024-01-11"},
            {"['millis_to_tz()', 1704897074000, +690, 'invalid']", "2024-01-11T02:01:14+11:30"},
            {"['millis_to_tz()', 1704897074000, +690, '11:11:11Z']", "02:01:14+11:30"},
            {"['millis_to_tz()', 1704897074000, +690, '11:11:11-05:00']", "02:01:14+11:30"},
    });
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query date diff string", "[Query][CBL-59]") {
    SECTION("Basic") {
        testExpressions({
                {"['date_diff_str()', '2018-01-31T00:00:00.001Z', '2018-01-31T00:00:00Z', 'millisecond']",
                 int64_t(1ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00.010Z', '2018-01-31T00:00:00Z', 'millisecond']",
                 int64_t(10ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00.100Z', '2018-01-31T00:00:00Z', 'millisecond']",
                 int64_t(100ll)},
                {"['date_diff_str()', '2018-01-31T00:00:01.5Z', '2018-01-31T00:00:00Z', 'millisecond']",
                 int64_t(1500ll)},
                {"['date_diff_str()', '2018-01-31T00:00:01.5Z', '2018-01-31T00:00:00Z', 'second']", int64_t(1ll)},
                {"['date_diff_str()', '2018-01-31T00:01:01.5Z', '2018-01-31T00:00:00Z', 'second']", int64_t(61ll)},
                {"['date_diff_str()', '2018-01-31T00:01:01.5Z', '2018-01-31T00:00:00Z', 'minute']", int64_t(1ll)},
                {"['date_diff_str()', '2018-01-31T01:01:00.5Z', '2018-01-31T00:00:00Z', 'minute']", int64_t(61ll)},
                {"['date_diff_str()', '2018-01-31T01:00:01.5Z', '2018-01-31T00:00:00Z', 'hour']", int64_t(1ll)},
                {"['date_diff_str()', '2018-02-01T01:00:00.5Z', '2018-01-31T00:00:00Z', 'hour']", int64_t(25ll)},
                {"['date_diff_str()', '2018-01-02T01:00:01.5Z', '2018-01-01T00:00:00Z', 'day']", int64_t(1ll)},
                {"['date_diff_str()', '2018-03-01T01:00:01.5Z', '2018-02-01T00:00:00Z', 'day']", int64_t(28ll)},
                {"['date_diff_str()', '2016-03-01T01:00:01.5Z', '2016-02-01T00:00:00Z', 'day']", int64_t(29ll)},
                {"['date_diff_str()', '2018-02-01T01:00:00.5Z', '2018-01-01T00:00:00Z', 'day']", int64_t(31ll)},
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
        testExpressions({
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:00.001Z', 'millisecond']",
                 int64_t(-1ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:00.010Z', 'millisecond']",
                 int64_t(-10ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:00.100Z', 'millisecond']",
                 int64_t(-100ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:00:01.5Z', 'second']", int64_t(-1ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:01:01.5Z', 'second']", int64_t(-61ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T00:01:01.5Z', 'minute']", int64_t(-1ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T01:01:00.5Z', 'minute']", int64_t(-61ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-01-31T01:00:01.5Z', 'hour']", int64_t(-1ll)},
                {"['date_diff_str()', '2018-01-31T00:00:00Z', '2018-02-01T01:00:00.5Z', 'hour']", int64_t(-25ll)},
                {"['date_diff_str()', '2018-01-01T00:00:00Z', '2018-01-02T01:00:01.5Z', 'day']", int64_t(-1ll)},
                {"['date_diff_str()', '2018-02-01T00:00:00Z', '2018-03-01T01:00:01.5Z', 'day']", int64_t(-28ll)},
                {"['date_diff_str()', '2016-02-01T00:00:00Z', '2016-03-01T01:00:01.5Z', 'day']", int64_t(-29ll)},
                {"['date_diff_str()', '2018-01-01T00:00:00Z', '2018-02-01T01:00:00.5Z', 'day']", int64_t(-31ll)},
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
        testExpressions({
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

N_WAY_TEST_CASE_METHOD(QueryTest, "Query date diff millis", "[Query][CBL-59]") {
    SECTION("Basic") {
        testExpressions({{"['date_diff_millis()', 1517356800001, 1517356800000, 'millisecond']", int64_t(1ll)},
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
                         {"['date_diff_millis()', 29923779601500, -1672531200000, 'millennium']", int64_t(1ll)}});
    }

    SECTION("Negative") {
        testExpressions({{"['date_diff_millis()', 1517356800000, 1517356800001, 'millisecond']", int64_t(-1ll)},
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
                         {"['date_diff_millis()', -1672531200000, 29923779601500, 'millennium']", int64_t(-1ll)}});
    }

    SECTION("N1QL consistency") {
        testExpressions({
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

N_WAY_TEST_CASE_METHOD(QueryTest, "Query date add string", "[Query][CBL-59]") {
    SECTION("Basic") {
        testExpressions({
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
                // Without fmt, should always be ISO8601
                {"['date_add_str()', '2018-01-01', 1, 'century']", "2118-01-01T00:00:00Z"},
                // With fmt
                {"['date_add_str()', '2018-01-01', 1, 'century', '1111-11-11']", "2118-01-01"},

                // Note: Windows cannot handle times after year 3000
                {"['date_add_str()', '1918-01-01T00:00:00Z', 1, 'millennium']", "2918-01-01T00:00:00Z"},
        });
    }

    SECTION("Negative") {
        testExpressions({
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
        testExpressions(
                {{"['date_add_str()', '2018-01-01T00:00:00Z', 1500, 'millisecond']", "2018-01-01T00:00:01.500Z"},
                 {"['date_add_str()', '2018-01-01T00:00:00Z', 61, 'second']", "2018-01-01T00:01:01Z"},
                 {"['date_add_str()', '2018-01-01T00:00:00Z', 61, 'minute']", "2018-01-01T01:01:00Z"},
                 {"['date_add_str()', '2018-01-01T00:00:00Z', 25, 'hour']", "2018-01-02T01:00:00Z"},
                 {"['date_add_str()', '2018-01-01T00:00:00Z', 31, 'day']", "2018-02-01T00:00:00Z"},
                 {"['date_add_str()', '2018-02-27T00:00:00Z', 1, 'week']", "2018-03-06T00:00:00Z"},
                 {"['date_add_str()', '2018-01-01T00:00:00Z', 12, 'month']", "2019-01-01T00:00:00Z"},
                 {"['date_add_str()', '2018-01-01T00:00:01.500Z', -1500, 'millisecond']", "2018-01-01T00:00:00Z"},
                 {"['date_add_str()', '2018-01-01T00:01:01Z', -61, 'second']", "2018-01-01T00:00:00Z"},
                 {"['date_add_str()', '2018-01-01T01:01:00Z', -61, 'minute']", "2018-01-01T00:00:00Z"},
                 {"['date_add_str()', '2018-01-02T01:00:00Z', -25, 'hour']", "2018-01-01T00:00:00Z"},
                 {"['date_add_str()', '2018-02-01T00:00:00Z', -31, 'day']", "2018-01-01T00:00:00Z"},
                 {"['date_add_str()', '2019-01-01T00:00:00Z', -12, 'month']", "2018-01-01T00:00:00Z"}});
    }

    SECTION("Special cases") {
        testExpressions({
                // Leap year
                {"['date_add_str()', '2018-02-28T00:00:00Z', 1, 'day']", "2018-03-01T00:00:00Z"},
                {"['date_add_str()', '2016-02-28T00:00:00Z', 1, 'day']", "2016-02-29T00:00:00Z"},

                // Keep time offset
                {"['date_add_str()', '2016-01-01T00:00:00-07:00', 1, 'day']", "2016-01-02T00:00:00-07:00"},

                // Short month
                {"['date_add_str()', '2018-02-15T00:00:00Z', 1, 'month']", "2018-03-15T00:00:00Z"},

                // Questionable result, but matches N1QL server
                {"['date_add_str()', '2018-01-31T00:00:00Z', 1, 'month']", "2018-03-03T00:00:00Z"},
        });
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query date add millis", "[Query][CBL-59]") {
    SECTION("Basic") {
        testExpressions({
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
        testExpressions({{"['date_add_millis()', 1514764800001, -1, 'millisecond']", int64_t(1514764800000)},
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
                         {"['date_add_millis()', 31556995200000, -1, 'millennium']", int64_t(0)}});
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query unsigned", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeUInt(1);
        });
        t.commit();
    }

    auto                      query = store->compileQuery(json5("{'WHAT': ['.num']}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asUnsigned() == 1U);
    CHECK(e->columns()[0]->asInt() == 1);
}

// Test for #341, "kData fleece type unable to be queried"
N_WAY_TEST_CASE_METHOD(QueryTest, "Query data type", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeData("number one"_sl);
        });
        t.commit();
    }

    auto                      query = store->compileQuery(json5("{'WHAT': ['.num']}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asData() == "number one"_sl);

    query = store->compileQuery(json5("{'WHAT': [['type()', ['.num']]]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "binary"_sl);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query Missing columns", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeInt(1234);
            enc.writeKey("string");
            enc.writeString("FOO");
        });
        t.commit();
    }

    auto                      query = store->compileQuery(json5("{'WHAT': ['.num', '.string']}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->missingColumns() == 0);
    CHECK(e->columns()[0]->toJSONString() == "1234");
    CHECK(e->columns()[1]->toJSONString() == "\"FOO\"");

    query = store->compileQuery(json5("{'WHAT': ['.bogus', '.num', '.nope', '.string', '.gone']}"));
    e     = (query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->missingColumns() == 0x15);  // binary 10101, i.e. cols 0, 2, 4 are missing
    CHECK(e->columns()[1]->toJSONString() == "1234");
    CHECK(e->columns()[3]->toJSONString() == "\"FOO\"");
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Negative Limit / Offset", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        writeDoc("rec_001"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("num");
            enc.writeInt(1234);
            enc.writeKey("string");
            enc.writeString("FOO");
        });
        t.commit();
    }

    auto                      query = store->compileQuery(json5("{'WHAT': ['.num', '.string'], 'LIMIT': -1}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    CHECK(e->getRowCount() == 0);

    query = store->compileQuery(json5("{'WHAT': ['.num', '.string'], 'LIMIT': 100, 'OFFSET': -1}"));
    e     = (query->createEnumerator());
    CHECK(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSONString() == "1234");
    CHECK(e->columns()[1]->toJSONString() == "\"FOO\"");

    Query::Options opts(R"({"lim": -1})"_sl);
    query = store->compileQuery(json5("{'WHAT': ['.num', '.string'], 'LIMIT': ['$lim']}"));
    e     = (query->createEnumerator(&opts));
    CHECK(e->getRowCount() == 0);

    Query::Options opts2(R"({"lim": 100, "skip": -1})"_sl);
    query = store->compileQuery(json5("{'WHAT': ['.num', '.string'], 'LIMIT': ['$lim'], 'OFFSET': ['$skip']}"));
    e     = (query->createEnumerator(&opts2));
    CHECK(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSONString() == "1234");
    CHECK(e->columns()[1]->toJSONString() == "\"FOO\"");

    // Offset without limit:
    Query::Options opts3(R"({"skip": 0})"_sl);
    query = store->compileQuery(json5("{'WHAT': ['.num', '.string'], 'OFFSET': ['$skip']}"));
    e     = (query->createEnumerator(&opts3));
    CHECK(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->toJSONString() == "1234");
    CHECK(e->columns()[1]->toJSONString() == "\"FOO\"");
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query JOINs", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        string               docID = "rec-00";

        for ( int i = 0; i < 10; i++ ) {
            stringstream ss(docID);
            ss << i + 1;

            writeDoc(slice(ss.str()), DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("num1");
                enc.writeInt(i);
                enc.writeKey("num2");
                enc.writeInt(10 - i);
            });
        }

        writeDoc("magic"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("theone");
            enc.writeInt(4);
        });

        t.commit();
    }

    auto query = store->compileQuery(json5("{'WHAT': [['.main.num1']], 'FROM': [{'AS':'main'}, {'AS':'secondary', "
                                           "'ON': ['=', ['.main.num1'], ['.secondary.theone']]}]}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 4);

    query = store->compileQuery(
            json5("{'WHAT': [['.main.num1'], ['.secondary.theone']], 'FROM': [{'AS':'main'}, {'AS':'secondary', 'ON': "
                  "['=', ['.main.num1'], ['.secondary.theone']], 'JOIN':'LEFT OUTER'}]}"));
    e = (query->createEnumerator());
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
    e = (query->createEnumerator());
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

    explicit ArrayQueryTest(int option) : QueryTest(option) {}

    void checkQuery(int docNo, int expectedRowCount) {
        Retained<QueryEnumerator> e(query->createEnumerator());
        CHECK(e->getRowCount() == expectedRowCount);
        while ( e->next() ) {
            auto   cols          = e->columns();
            slice  docID         = cols[0]->asString();
            string expectedDocID = stringWithFormat("rec-%03d", docNo);
            CHECK(docID == slice(expectedDocID));
            ++docNo;
        }
    }

    void testArrayQuery(const string& json, bool checkOptimization) {
        addArrayDocs(1, 90);

        query              = store->compileQuery(json);
        string explanation = query->explain();
        Log("%s", explanation.c_str());
        checkQuery(88, 3);

        Log("-------- Creating index --------");
        store->createIndex("numbersIndex"_sl, R"([[".numbers"]])", IndexSpec::kArray);
        Log("-------- Recompiling query with index --------");
        query = store->compileQuery(json);
        checkOptimized(query, checkOptimization);
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

N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query ANY", "[Query]") {
    testArrayQuery(json5("['SELECT', {\
                             WHERE: ['ANY', 'num', ['.numbers'],\
                                            ['=', ['?num'], 'eight-eight']]}]"),
                   false);
}

N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query UNNEST", "[Query]") {
    testArrayQuery(json5("['SELECT', {\
                              FROM: [{as: 'doc'}, \
                                     {as: 'num', 'unnest': ['.doc.numbers']}],\
                              WHERE: ['=', ['.num'], 'eight-eight']}]"),
                   true);
}

N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query ANY expression", "[Query]") {
    addArrayDocs(1, 90);

    auto json          = json5("['SELECT', {\
                          WHERE: ['ANY', 'num', ['[]', ['.numbers[0]'], ['.numbers[1]']],\
                                         ['=', ['?num'], 'eight']]}]");
    query              = store->compileQuery(json);
    string explanation = query->explain();
    Log("%s", explanation.c_str());

    checkQuery(12, 2);
}

N_WAY_TEST_CASE_METHOD(ArrayQueryTest, "Query UNNEST expression", "[Query]") {
    addArrayDocs(1, 90);

    auto json          = json5("['SELECT', {\
                              FROM: [{as: 'doc'}, \
                                     {as: 'num', 'unnest': ['[]', ['.doc.numbers[0]'], ['.doc.numbers[1]']]}],\
                              WHERE: ['=', ['.num'], 'one-eight']}]");
    query              = store->compileQuery(json);
    string explanation = query->explain();
    Log("%s", explanation.c_str());

    checkQuery(22, 2);

    if ( GENERATE(0, 1) ) {
        Log("-------- Creating JSON index --------");
        store->createIndex("numbersIndex"_sl, json5("[['[]', ['.numbers[0]'], ['.numbers[1]']]]"), IndexSpec::kArray);
    } else {
        Log("-------- Creating N1QL index --------");
        store->createIndex("numbersIndex"_sl, "[numbers[0], numbers[1]]", QueryLanguage::kN1QL, IndexSpec::kArray);
    }
    Log("-------- Recompiling query with index --------");
    query = store->compileQuery(json);
    checkOptimized(query);

    checkQuery(22, 2);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query nested ANY of dict", "[Query]") {  // CBL-1248
    ExclusiveTransaction t(store->dataFile());

    for ( int i = 0; i < 2; i++ ) {
        string docID = stringWithFormat("rec-%03d", i + 1);
        writeDoc(docID, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("variants");
            enc.beginArray(1);
            enc.beginDictionary(1);
            enc.writeKey("items");
            enc.beginArray(1);
            enc.beginDictionary(1);
            enc.writeKey("id");
            enc.writeInt(i + 1);
            enc.endDictionary();
            enc.endArray();
            enc.endDictionary();
            enc.endArray();
        });
    }

    Retained<Query> q;
    vector<slice>   expectedResults;
    q = store->compileQuery(json5("{WHAT: ['._id'], \
                                  WHERE: ['ANY', 'V', ['.variants'], ['ANY', 'I', ['?V.items'], ['=', ['?I.id'], 2]]]}"));
    expectedResults.emplace_back("rec-002"_sl);

    Retained<QueryEnumerator> e(q->createEnumerator());
    REQUIRE(e->getRowCount() == expectedResults.size());
    size_t row = 0;
    for ( const auto& expectedResult : expectedResults ) {
        REQUIRE(e->next());
        CHECK(e->columns()[0]->asString() == expectedResult);
        ++row;
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query NULL/MISSING check", "[Query][N1QL]") {
    {
        ExclusiveTransaction t(store->dataFile());
        string               docID = "rec-00";

        for ( int i = 0; i < 3; i++ ) {
            stringstream ss;
            ss << "rec-0" << i + 1;
            writeDoc(slice(ss.str()), DocumentFlags::kNone, t, [=](Encoder& enc) {
                if ( i > 0 ) {
                    enc.writeKey("callsign");
                    if ( i == 1 ) {
                        enc.writeNull();
                    } else {
                        enc.writeString("ANA");
                    }
                }
            });
        }

        t.commit();
    }

    // SELECT meta.id WHERE callsign IS MISSING
    auto query = store->compileQuery(json5("{'WHAT':[['._id']],'WHERE':['IS',['.callsign'],['MISSING']]}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    CHECK(e->getRowCount() == 1);
    CHECK(e->next());
    CHECK(e->columns()[0]->asString() == "rec-01"_sl);

    query = store->compileQuery(json5("{'WHAT':[['._id']],'WHERE':['IS',['.callsign'],null]}"));
    e     = query->createEnumerator();
    CHECK(e->getRowCount() == 1);
    CHECK(e->next());
    CHECK(e->columns()[0]->asString() == "rec-02"_sl);

    query = store->compileQuery(string("SELECT meta().id FROM ") + collectionName + " WHERE callsign IS 'ANA'",
                                litecore::QueryLanguage::kN1QL);
    e     = query->createEnumerator();
    CHECK(e->getRowCount() == 1);
    CHECK(e->next());
    CHECK(e->columns()[0]->asString() == "rec-03"_sl);

    // SELECT meta.id WHERE callsign IS NOT VALUED
    query = store->compileQuery(json5("{'WHAT':[['._id']],'WHERE':['NOT',['IS VALUED',['.callsign']]]}"));
    e     = query->createEnumerator();
    CHECK(e->getRowCount() == 2);
    CHECK(e->next());
    CHECK(e->columns()[0]->asString() == "rec-01"_sl);
    CHECK(e->next());
    CHECK(e->columns()[0]->asString() == "rec-02"_sl);

    // SELECT meta.id WHERE callsign IS VALUED
    query = store->compileQuery(string("SELECT META().id FROM ") + collectionName + " WHERE callsign IS VALUED",
                                litecore::QueryLanguage::kN1QL);
    e     = query->createEnumerator();
    CHECK(e->getRowCount() == 1);
    CHECK(e->next());
    CHECK(e->columns()[0]->asString() == "rec-03"_sl);

    query = store->compileQuery(
            json5("{'WHAT': [['COUNT()','.'], ['.callsign']], 'WHERE':['IS NOT', ['.callsign'], null]}"));
    e = query->createEnumerator();
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
    CHECK(e->columns()[1]->asString() == "ANA"_sl);

    query = store->compileQuery(
            json5("{'WHAT': [['COUNT()','.'], ['.callsign']], 'WHERE':['IS', ['.callsign'], null]}"));
    e = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 1);
    CHECK(e->columns()[1]->asString().buf == nullptr);

    query = store->compileQuery(json5("{'WHAT': [['.callsign']]}"));
    e     = (query->createEnumerator());
    CHECK(e->getRowCount() == 3);  // Make sure there are actually three docs!
}

// NOTE: This test cannot be reproduced in this way on Windows, and it is likely a Unix specific
// problem.  Leaving an enumerator open in this way will cause a permission denied error when
// trying to delete the database via db->deleteDataFile()
#ifndef _MSC_VER
N_WAY_TEST_CASE_METHOD(QueryTest, "Query finalized after db deleted", "[Query]") {
    Retained<Query> query{
            store->compileQuery(json5("{WHAT: ['.num', ['*', ['.num'], ['.num']]], WHERE: ['>', ['.num'], 10]}"))};
    Retained<QueryEnumerator> e(query->createEnumerator());
    e->next();
    query = nullptr;

    deleteDatabase();

    // Now free the query enum, which will free the sqlite_stmt, triggering a SQLite warning
    // callback about the database file being unlinked:
    e = nullptr;

    // Assert that the callback did not log a warning:
    CHECK(warningsLogged() == 0);
}
#endif


TEST_CASE_METHOD(QueryTest, "Query deleted docs", "[Query]") {
    addNumberedDocs(1, 10);
    {
        ExclusiveTransaction t(store->dataFile());
        for ( int i = 11; i <= 20; i++ )
            writeNumberedDoc(i, nullslice, t, DocumentFlags::kDeleted | DocumentFlags::kHasAttachments);
        t.commit();
    }

    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['<=', ['.num'], 15]}")) == 10);
    // Different ways to express that the query should apply to deleted docs only:
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['AND', ['<=', ['.num'], 15], ['._deleted']]}")) == 5);
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['OR', ['<=', ['.num'], 5], ['._deleted']]}")) == 15);
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['=', ['._deleted'], true]}")) == 10);
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['._deleted']}")) == 10);
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['.', '_deleted']}")) == 10);
    CHECK(rowsInQuery(json5("{WHAT: [ '._id'], WHERE: ['_.', ['meta()'], 'deleted']}")) == 10);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query expiration", "[Query]") {
    addNumberedDocs(1, 3);
    expiration_t now = KeyStore::now();

    {
        Retained<Query>           query{store->compileQuery(json5("{WHAT: ['._id'], WHERE: ['.expiration']}"))};
        Retained<QueryEnumerator> e(query->createEnumerator());
        CHECK(!e->next());
    }

    store->setExpiration("rec-001"_sl, now - 10000);
    store->setExpiration("rec-003"_sl, now + 10000);

    {
        Retained<Query>           query{store->compileQuery(json5("{WHAT: ['._expiration'], ORDER_BY: [['._id']]}"))};
        Retained<QueryEnumerator> e(query->createEnumerator());
        CHECK(e->next());
        CHECK(expiration_t(e->columns()[0]->asInt()) == now - 10000);
        CHECK(e->next());
        CHECK(e->columns()[0]->type() == kNull);
        CHECK(e->missingColumns() == 1);
        CHECK(e->next());
        CHECK(expiration_t(e->columns()[0]->asInt()) == now + 10000);
        CHECK(!e->next());
    }
    {
        Retained<Query> query{store->compileQuery(
                json5("{WHAT: ['._id'], WHERE: ['<=', ['._expiration'], ['$NOW']], ORDER_BY: [['._expiration']]}"))};

        Query::Options options{alloc_slice(format("{\"NOW\": %lld}", (long long)now))};

        Retained<QueryEnumerator> e(query->createEnumerator(&options));
        CHECK(e->next());
        CHECK(e->columns()[0]->asString() == "rec-001"_sl);
        CHECK(!e->next());
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query Dictionary Literal", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        string               docID = "rec-00";

        stringstream ss(docID);
        writeDoc(slice(ss.str()), DocumentFlags::kNone, t, [=](Encoder& enc) {
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

    auto query = store->compileQuery(json5("{'WHAT': [{ \
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

    Retained<QueryEnumerator> e(query->createEnumerator());
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

TEST_CASE_METHOD(QueryTest, "Test result alias", "[Query]") {
    if ( GENERATE(false, true) ) {
        logSection("secondary collection");
        store = &db->getKeyStore(".secondary");
    }

    ExclusiveTransaction t(store->dataFile());
    writeDoc("uber_doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("dict");
        enc.beginDictionary(2);
        enc.writeKey("key1");
        enc.writeInt(1);
        enc.writeKey("key2");
        enc.writeInt(2);
        enc.endDictionary();
        enc.writeKey("arr");
        enc.beginArray(2);
        enc.writeInt(1);
        enc.writeInt(2);
        enc.endArray();
    });

    writeDoc("uber_doc2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("dict");
        enc.beginDictionary(2);
        enc.writeKey("key1");
        enc.writeInt(2);
        enc.writeKey("key2");
        enc.writeInt(1);
        enc.endDictionary();
        enc.writeKey("arr");
        enc.beginArray(2);
        enc.writeInt(2);
        enc.writeInt(1);
        enc.endArray();
    });

    Retained<Query> q;
    vector<slice>   expectedResults;
    vector<string>  expectedAliases;
    SECTION("WHERE alias numeric literal") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', 1.375, 'answer']], \
            WHERE: ['=', ['.dict.key1'], 1]}"));
        expectedResults.emplace_back("uber_doc1"_sl);
        expectedAliases.emplace_back("1.375");
    }

    SECTION("WHERE alias string literal") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', 'Cthulhu ftaghn', 'answer']], \
            WHERE: ['=', ['.dict.key1'], 1]}"));
        expectedResults.emplace_back("uber_doc1"_sl);
        expectedAliases.emplace_back("\"Cthulhu ftaghn\"");
    }

    SECTION("WHERE alias as-is") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.dict.key2'], 'answer']], \
            WHERE: ['=', ['.answer'], 1]}"));
        expectedResults.emplace_back("uber_doc2"_sl);
    }

    SECTION("WHERE alias that shadows property") {
        // Here the alias is the same as the property used to define it...
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.dict.key2'], 'dict']], \
            WHERE: ['=', ['.dict'], 1]}"));
        expectedResults.emplace_back("uber_doc2"_sl);
    }

    SECTION("WHERE explict db alias precludes result alias") {
        // Here the alias is the same as the property used to define it...
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.dict.key2'], 'dict']], \
            FROM: [{AS: 'db', COLLECTION: '_'}], \
            WHERE: ['=', ['.db.dict'], 1]}"));
        // expect empty result
    }

    SECTION("WHERE alias with special name") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.dict'], 'name.with [special]']], \
            WHERE: ['=', ['.name\\\\.with \\\\[special\\\\].key1'], 1]}"));
        expectedResults.emplace_back("uber_doc1"_sl);
    }

    SECTION("WHERE key on alias") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.dict'], 'answer']], \
            WHERE: ['=', ['.answer.key1'], 1]}"));
        expectedResults.emplace_back("uber_doc1"_sl);
    }

    SECTION("WHERE index on alias") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.arr'], 'answer']], \
            WHERE: ['=', ['.answer[1]'], 1]}"));
        expectedResults.emplace_back("uber_doc2"_sl);
    }

    SECTION("ORDER BY alias as-is") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.dict.key2'], 'answer']], \
            ORDER_BY: [['.answer']]}"));
        expectedResults.emplace_back("uber_doc2"_sl);
        expectedResults.emplace_back("uber_doc1"_sl);
    }

    SECTION("ORDER BY key on alias") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.dict'], 'name.with [special]']], \
            ORDER_BY: [['DESC', ['.name\\\\.with \\\\[special\\\\].key1']]]}"));
        expectedResults.emplace_back("uber_doc2"_sl);
        expectedResults.emplace_back("uber_doc1"_sl);
    }

    SECTION("ORDER BY index on alias") {
        q = store->compileQuery(json5("{WHAT: ['._id', \
            ['AS', ['.arr'], 'answer']], \
            ORDER_BY: [['.answer[1]']]}"));
        expectedResults.emplace_back("uber_doc2"_sl);
        expectedResults.emplace_back("uber_doc1"_sl);
    }

    Retained<QueryEnumerator> e(q->createEnumerator());
    REQUIRE(e->getRowCount() == expectedResults.size());
    size_t row = 0;
    for ( const auto& expectedResult : expectedResults ) {
        REQUIRE(e->next());
        CHECK(e->columns()[0]->asString() == expectedResult);
        if ( !expectedAliases.empty() ) CHECK(e->columns()[1]->toJSONString() == expectedAliases[row]);
        ++row;
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query N1QL", "[Query][N1QL]") {
    addNumberedDocs();
    Retained<Query> query{store->compileQuery(string("SELECT num, num*num FROM ") + collectionName
                                                      + " WHERE num >= 30 and num <= 40 ORDER BY num",
                                              QueryLanguage::kN1QL)};
    CHECK(query->columnCount() == 2);
    int                       num = 30;
    Retained<QueryEnumerator> e(query->createEnumerator());
    while ( e->next() ) {
        string expectedDocID = stringWithFormat("rec-%03d", num);
        auto   cols          = e->columns();
        REQUIRE(cols.count() == 2);
        REQUIRE(cols[0]->asInt() == num);
        REQUIRE(cols[1]->asInt() == num * num);
        ++num;
    }
    REQUIRE(num == 41);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query closes when db closes", "[Query]") {
    // Tests fix for <https://issues.couchbase.com/browse/CBL-214>
    addNumberedDocs(1, 10);

    Retained<Query>           query = store->compileQuery(json5("{WHAT: [ '._id'], WHERE: ['>=', ['.num'], 5]}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    CHECK(e->getRowCount() == 6);

    // Close & delete the database while the Query and QueryEnumerator still exist:
    deleteDatabase();
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query Math Precision", "[Query]") {
    addNumberedDocs();
    Retained<Query> query;
    query = store->compileQuery(json5("{WHAT: ['.num', ['AS', ['/', 5.0, 15.0], 'd1'], ['AS', ['/', 5.5, 16.5], 'd2'], "
                                      "['AS', ['/', 5, 15], 'd3']]}"));

    CHECK(query->columnTitles() == (vector<string>{"num", "d1", "d2", "d3"}));
    Retained<QueryEnumerator> e(query->createEnumerator());

    while ( e->next() ) {
        auto cols = e->columns();
        REQUIRE(cols.count() == 4);
        REQUIRE(cols[2]->asDouble() == (double)5.5 / 16.5);
        REQUIRE(cols[1]->asDouble() == (double)5 / 15);
        // This integer division is on purpose (see above query).
        REQUIRE(cols[3]->asDouble() == 5 / 15);  // NOLINT(bugprone-integer-division)
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query Special Chars", "[Query]") {
    vector<string>       keys{"$Type", "Ty$pe", "Type$"};
    ExclusiveTransaction t(store->dataFile());
    writeDoc("doc"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        for ( const string& key : keys ) {
            enc.writeKey(key);
            enc.writeString("special");
        }
    });
    t.commit();

    for ( const string& key : keys ) {
        string                    queryStr = stringWithFormat("{WHAT: [['.%s']]}", key.c_str());
        Retained<Query>           query    = store->compileQuery(json5(queryStr));
        Retained<QueryEnumerator> e(query->createEnumerator());
        INFO("Attempted with array syntax " << key);
        REQUIRE(e->next());
        CHECK(e->columns()[0]->asString() == "special"_sl);

        queryStr = stringWithFormat("{WHAT: ['.%s']}", key.c_str());
        query    = store->compileQuery(json5(queryStr));
        e        = query->createEnumerator();
        INFO("Attempted with string syntax " << key);
        REQUIRE(e->next());
        CHECK(e->columns()[0]->asString() == "special"_sl);
    }
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query Special Chars Alias", "[Query][N1QL]") {
    ExclusiveTransaction t(store->dataFile());
    writeDoc("doc-01"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("customerId");
        enc.writeString("Jack");
        enc.writeKey("test_id");
        enc.writeString("alias_func");
    });
    writeDoc("doc-02"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("customerId");
        enc.writeString("Jean");
        enc.writeKey("test_id");
        enc.writeString("alias_func");
    });
    writeDoc("doc-03"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("customerId");
        enc.writeString("Scott");
        enc.writeKey("test_id");
        enc.writeString("alias_func");
    });
    t.commit();

    Retained<Query> query;
    int             SWITCH = GENERATE(0, 1);
    switch ( SWITCH ) {
        case 0:
            {
                string queryStr = string("SELECT customerId AS `$1` FROM ") + collectionName
                                  + " WHERE test_id='alias_func' ORDER BY `$1` LIMIT 2";
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
                CHECK(query->columnTitles() == vector<string>{"$1"});
            }
            break;
        case 1:
            {
                // collectionName = "_" | "_default.Secondary" | "scopey.subsidiary"
                string queryStr = "SELECT "s + collectionName + ".customerId AS `scope.collection` FROM "
                                  + collectionName + " WHERE test_id='alias_func' ORDER BY `scope.collection` LIMIT 2";
                cout << queryStr << endl;
                query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
                CHECK(query->columnTitles() == vector<string>{"scope.collection"});
            }
            break;
        default:
            break;
    }

    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "Jack"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "Jean"_sl);
    REQUIRE(!e->next());
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query N1QL ARRAY_AGG", "[Query][N1QL]") {
    ExclusiveTransaction t(store->dataFile());
    writeDoc("doc-01"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("customerId");
        enc.writeString("Jack");
        enc.writeKey("test_id");
        enc.writeString("agg_func");
    });
    writeDoc("doc-02"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("customerId");
        enc.writeString("Jean");
        enc.writeKey("test_id");
        enc.writeString("alias_func");
    });
    writeDoc("doc-03"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
        enc.writeKey("customerId");
        enc.writeString("Scott");
        enc.writeKey("test_id");
        enc.writeString("agg_func");
    });
    t.commit();

    string n1ql = string("SELECT array_Agg(customerId) FROM ") + collectionName + " where test_id = \"agg_func\"";
    Retained<Query>           query = store->compileQuery(n1ql, QueryLanguage::kN1QL);
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->next());
    CHECK(e->columns().count() == 1);
    CHECK(e->columns()[0]->type() == kArray);
    const Array* agg = e->columns()[0]->asArray();
    CHECK(agg->count() == 2);
    CHECK(agg->get(0)->asString() == "Jack"_sl);
    CHECK(agg->get(1)->asString() == "Scott"_sl);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query META", "[Query][N1QL]") {
    {
        ExclusiveTransaction t(store->dataFile());
        string               docID = "doc1";
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeNull();
            enc.writeKey("real_value");
            enc.writeInt(1);
        });
        writeDoc("doc2"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("value");
            enc.writeNull();
            enc.writeKey("atai");
            enc.writeInt(1);
        });
        t.commit();
    }

    Retained<Query> query{store->compileQuery(string("SELECT meta() FROM ") + collectionName, QueryLanguage::kN1QL)};
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    const Value* dict = e->columns()[0];
    REQUIRE(dict->type() == kDict);
    string dictJson = dict->toJSON().asString();
    transform(dictJson.begin(), dictJson.end(), dictJson.begin(), [](char c) { return c == '"' ? '\'' : c; });
    CHECK(dictJson == "{'deleted':0,'id':'doc1','sequence':1}");

    string collectionAlias = collectionName;
    if ( auto dot = collectionAlias.find('.'); dot != string::npos ) collectionAlias = "`"s + collectionAlias + "`";

    query = store->compileQuery("SELECT meta(" + collectionAlias + ") from " + collectionName, QueryLanguage::kN1QL);
    e     = query->createEnumerator();
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    dict = e->columns()[0];
    REQUIRE(dict->type() == kDict);
    dictJson = dict->toJSON().asString();
    transform(dictJson.begin(), dictJson.end(), dictJson.begin(), [](char c) { return c == '"' ? '\'' : c; });
    CHECK(dictJson == "{'deleted':0,'id':'doc1','sequence':1}");

    query = store->compileQuery(string("SELECT meta().id FROM ") + collectionName, QueryLanguage::kN1QL);
    e     = query->createEnumerator();
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "doc2"_sl);

    query = store->compileQuery("SELECT meta(" + collectionAlias + ").id from " + collectionName, QueryLanguage::kN1QL);
    e     = query->createEnumerator();
    REQUIRE(e->getRowCount() == 2);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "doc1"_sl);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asString() == "doc2"_sl);
}

TEST_CASE_METHOD(QueryTest, "Various Exceptional Conditions", "[Query]") {
    {
        ExclusiveTransaction t(store->dataFile());
        string               docID = "doc1";
        writeDoc("doc1"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("unitPrice");
            enc.writeInt(8);
            enc.writeKey("orderlines");
            enc.beginArray();
            enc.writeInt(1);
            enc.writeInt(2);
            enc.endArray();
        });
        t.commit();
    }

    string meta_default = "META(" + collectionName + ").revisionID";
    std::tuple<const char*, std::function<bool(const Value*, bool)>> testCases[] = {{"acos(3)",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =NULL
                                                                                         return !missing
                                                                                                && v->type() == kNull;
                                                                                     }},
                                                                                    {"acos(\"abc\")",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =NULL
                                                                                         return !missing
                                                                                                && v->type() == kNull;
                                                                                     }},
                                                                                    {"2/0",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =NULL
                                                                                         return missing;
                                                                                     }},
                                                                                    {"lower([1,2])",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =NULL
                                                                                         return !missing
                                                                                                && v->type() == kNull;
                                                                                     }},
                                                                                    /*4*/
                                                                                    {"length(missingValue)",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =MISSING
                                                                                         return missing
                                                                                                && v->type() == kNull;
                                                                                     }},
                                                                                    {"is_array(null)",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =NULL
                                                                                         return !missing
                                                                                                && v->type() == kNull;
                                                                                     }},
                                                                                    {"atan(asin(1.1))",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =NULL
                                                                                         return !missing
                                                                                                && v->type() == kNull;
                                                                                     }},
                                                                                    {"round(12.5)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =13
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 13;
                                                                                     }},
                                                                                    {"8/10",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =0
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 0;
                                                                                     }},
                                                                                    /*9*/
                                                                                    {"unitPrice/10",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =0
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 0;
                                                                                     }},
                                                                                    {"orderlines",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // type() == kArray & columnTitle="orderlines"
                                                                                         return !missing
                                                                                                && v->type() == kArray;
                                                                                     }},
                                                                                    {"orderlines[0]",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // columnTitle="$11"
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 1;
                                                                                     }},
                                                                                    {"div(8, 10)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =0.8
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 0.8;
                                                                                     }},
                                                                                    {"idiv(8, 10)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =0
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 0;
                                                                                     }},
                                                                                    /*14*/
                                                                                    {"idiv(-1, 1.9)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =-1
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == -1;
                                                                                     }},
                                                                                    {"idiv(-1, 2.0)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =0
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 0;
                                                                                     }},
                                                                                    {"idiv(-1, 2.9)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =0
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 0;
                                                                                     }},
                                                                                    {"idiv(-3.9, 2.1)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =-1
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == -1;
                                                                                     }},
                                                                                    {"idiv(5, 3)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =1
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 1;
                                                                                     }},
                                                                                    /*19*/
                                                                                    {"idiv(5, 3.0)",
                                                                                     [](const Value* v,
                                                                                        bool         missing) {  // =1
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 1;
                                                                                     }},
                                                                                    {"idiv(1, 0.99)",
                                                                                     [](const Value* v,
                                                                                        bool missing) {  // =NULL
                                                                                         return !missing
                                                                                                && v->type() == kNull;
                                                                                     }},
                                                                                    {"round_even(12.5)",
                                                                                     [](const Value* v, bool missing) {
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 12;
                                                                                     }},
                                                                                    {"round_even(11.5)",
                                                                                     [](const Value* v, bool missing) {
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble() == 12;
                                                                                     }},
                                                                                    {"round_even(12.115, 2)",
                                                                                     [](const Value* v, bool missing) {
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble()
                                                                                                           == 12.12;
                                                                                     }},
                                                                                    /*24*/
                                                                                    {"round_even(-12.125, 2)",
                                                                                     [](const Value* v, bool missing) {
                                                                                         return !missing
                                                                                                && v->type() == kNumber
                                                                                                && v->asDouble()
                                                                                                           == -12.12;
                                                                                     }},
                                                                                    {"META().id",
                                                                                     [](const Value* v, bool missing) {
                                                                                         return !missing
                                                                                                && v->type() == kString
                                                                                                && (v->asString()
                                                                                                            .compare(
                                                                                                                    "do"
                                                                                                                    "c"
                                                                                                                    "1")
                                                                                                    == 0);
                                                                                     }},
                                                                                    {meta_default.c_str(),
                                                                                     [](const Value* v, bool missing) {
                                                                                         return missing
                                                                                                && v->type() == kNull;
                                                                                     }}};
    size_t testCaseCount = sizeof(testCases) / sizeof(testCases[0]);
    string queryStr      = "select ";
    queryStr += std::get<0>(testCases[0]);
    for ( unsigned i = 1; i < testCaseCount; ++i ) { (queryStr += ", ") += std::get<0>(testCases[i]); }
    queryStr += " from " + collectionName;

    Retained<Query>           query = store->compileQuery(queryStr, QueryLanguage::kN1QL);
    Retained<QueryEnumerator> e     = query->createEnumerator();
    REQUIRE(query->columnTitles()[9] == "$10");
    REQUIRE(query->columnTitles()[10] == "orderlines");
    REQUIRE(query->columnTitles()[11] == "$11");
    REQUIRE(query->columnTitles()[25] == "id");
    REQUIRE(query->columnTitles()[26] == "revisionID");
    REQUIRE(e->next());
    uint64_t missingColumns = e->missingColumns();
    for ( unsigned i = 0; i < testCaseCount; ++i ) {
        REQUIRE(std::get<1>(testCases[i])(e->columns()[i], missingColumns & (1ull << i)));
    }
}

TEST_CASE_METHOD(QueryTest, "Query cross-collection JOINs", "[Query]") {
    {
        ExclusiveTransaction t(db);
        string               docID = "rec-00";

        for ( int i = 0; i < 10; i++ ) {
            stringstream ss("rec-00");
            ss << i + 1;

            writeDoc(slice(ss.str()), DocumentFlags::kNone, t, [=](Encoder& enc) {
                enc.writeKey("num1");
                enc.writeInt(i);
                enc.writeKey("num2");
                enc.writeInt(10 - i);
            });
        }

        KeyStore& secondary = db->getKeyStore(".secondary");
        writeDoc(secondary, "magic"_sl, DocumentFlags::kNone, t, [=](Encoder& enc) {
            enc.writeKey("theone");
            enc.writeInt(4);
        });

        t.commit();
    }

    auto query = db->compileQuery(json5("{'WHAT': [['.main.num1']], 'FROM': [{'AS':'main'}, {'COLLECTION':'secondary', "
                                        "'ON': ['=', ['.main.num1'], ['.secondary.theone']]}]}"));
    Retained<QueryEnumerator> e(query->createEnumerator());
    REQUIRE(e->getRowCount() == 1);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 4);

    query = db->compileQuery(json5(
            "{'WHAT': [['.main.num1'], ['.secondary.theone']], 'FROM': [{'AS':'main'}, {'COLLECTION':'secondary', "
            "'ON': ['=', ['.main.num1'], ['.secondary.theone']], 'JOIN':'LEFT OUTER'}]}"));
    e     = (query->createEnumerator());
    REQUIRE(e->getRowCount() == 10);
    e->seek(4);
    CHECK(e->columns()[0]->asInt() == 4);
    CHECK(e->columns()[1]->asInt() == 4);
    REQUIRE(e->next());
    CHECK(e->columns()[0]->asInt() == 5);
    CHECK(e->columns()[1]->asInt() == 0);
}

TEST_CASE_METHOD(QueryTest, "Alternative FROM names", "[Query]") {
    addNumberedDocs(1, 10);

    auto checkType = [this](const string& jsonQuery) {
        Retained<Query>           query = store->compileQuery(jsonQuery);
        Retained<QueryEnumerator> e(query->createEnumerator());
        while ( e->next() ) { CHECK(e->columns()[0]->asString() == "number"_sl); }
    };

    auto checkTypeN1QL = [this](const string& n1qlQuery) {
        Retained<Query>           query = store->compileQuery(n1qlQuery, QueryLanguage::kN1QL);
        Retained<QueryEnumerator> e(query->createEnumerator());
        while ( e->next() ) { CHECK(e->columns()[0]->asString() == "number"_sl); }
    };

    checkType(json5("{'WHAT': ['.foo\\\\.bar.type'], 'FROM': [{'COLLECTION':'_', 'AS':'foo.bar'}]}"));
    checkType(json5("{'WHAT': ['.type'], 'FROM': [{'COLLECTION':'_default'}]}"));
    checkType(json5("{'WHAT': ['.type'], 'FROM': [{'COLLECTION':'_'}]}"));
    checkType(json5("{'WHAT': ['.type'], 'FROM': [{'COLLECTION':'" + databaseName() + "'}]}"));
    checkType(json5("{'WHAT': ['.foo.type'], 'FROM': [{'COLLECTION':'_', 'AS':'foo'}]}"));

    checkTypeN1QL("SELECT type FROM _");
    checkTypeN1QL("SELECT type FROM _default");
    checkTypeN1QL("SELECT type FROM _default._default");
    checkTypeN1QL("SELECT foo.type FROM _ AS foo");
    checkTypeN1QL("SELECT `foo.bar`.type FROM _ AS `foo.bar`");

    // For database names that include dot, we must properly quote it.
    // In JSON, we escape dot inside the JSON string value.
    // In N1QL expression, we back-quote quote it.

    _databaseName = "cbl.core.temp";
    checkType(json5(R"({'WHAT': ['.type'], 'FROM': [{'COLLECTION':'cbl\\.core\\.temp'}]})"));
    checkTypeN1QL("SELECT type FROM `cbl.core.temp`");
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Require FROM for N1QL expressions", "[Query]") {
    addNumberedDocs(1, 10);
    bool   withFrom = GENERATE(true, false);
    string queryStr = "select *";
    if ( withFrom ) {
        (queryStr += " from ") += collectionName;
        Retained<Query>           query{db->compileQuery(queryStr, QueryLanguage::kN1QL)};
        Retained<QueryEnumerator> e(query->createEnumerator());
        CHECK(e->getRowCount() == 10);
    } else {
        ExpectingExceptions _;
        CHECK_THROWS_WITH(db->compileQuery(queryStr, QueryLanguage::kN1QL), "N1QL error: missing the FROM clause");
    }
}

TEST_CASE_METHOD(QueryTest, "Invalid collection names", "[Query]") {
    string      tooLong(252, 'x');
    string      tooLong2 = "a." + tooLong, tooLong3 = tooLong + ".z";
    const char* kBadCollectionNames[] = {// "_",   <- nope, "_" happens to be legal (synonym for the default collection)
                                         "%",
                                         "%xx",
                                         "_xx",
                                         "x y",
                                         ".",
                                         "xx.",
                                         ".xx",
                                         "_b.c",
                                         "b._c",
                                         "in.val.id",
                                         "in..val",
                                         "foo._default",
                                         tooLong.c_str(),
                                         tooLong2.c_str(),
                                         tooLong3.c_str()};
    for ( auto badName : kBadCollectionNames ) {
        INFO("Collection name is " << badName);
        ExpectingExceptions expect;
        try {
            store->compileQuery(json5("{'WHAT': ['.'], 'FROM': [{'COLLECTION':'"s + badName + "'}]}"));
            FAIL_CHECK("Didn't detect an invalid collection name");
        } catch ( const error& x ) {
            CHECK(x == error::InvalidQuery);
            if ( string(x.what()).find("is not a valid collection") == string::npos )
                FAIL_CHECK("Wrong error: " << x.what());
        }
    }
}
