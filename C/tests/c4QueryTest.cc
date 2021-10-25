//
// c4QueryTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4QueryTest.hh"
#include "c4BlobStore.h"
#include "c4Collection.h"
#include "c4Observer.h"
#include "StringUtil.hh"
#include <thread>
using namespace std;


static bool operator==(C4FullTextMatch a, C4FullTextMatch b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static ostream& operator<< (ostream& o, C4FullTextMatch match) {
    return o << "{ds " << match.dataSource << ", prop " << match.property << ", term " << match.term << ", "
             << "bytes " << match.start << " + " << match.length << "}";
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query Basic", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    CHECK(run() == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));

    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"), "", true);
    CHECK(run("{\"offset\":1,\"limit\":8}") == (vector<string>{"0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
    CHECK(run("{\"offset\":1,\"limit\":4}") == (vector<string>{"0000015", "0000036", "0000043", "0000053"}));

    compile(json5("['AND', ['=', ['array_count()', ['.', 'contact', 'phone']], 2],\
                           ['=', ['.', 'gender'], 'male']]"));
    CHECK(run() == (vector<string>{"0000002", "0000014", "0000017", "0000027", "0000031", "0000033", "0000038", "0000039", "0000045", "0000047",
        "0000049", "0000056", "0000063", "0000065", "0000075", "0000082", "0000089", "0000094", "0000097"}));

    // MISSING means no value is present (at that array index or dict key)
    compile(json5("['IS', ['.', 'contact', 'phone', [0]], ['MISSING']]"), "", true);
    CHECK(run("{\"offset\":0,\"limit\":4}") == (vector<string>{"0000004", "0000006", "0000008", "0000015"}));

    // ...whereas null is a JSON null value
    compile(json5("['IS', ['.', 'contact', 'phone', [0]], null]"), "", true);
    CHECK(run("{\"offset\":0,\"limit\":4}") == (vector<string>{}));

    // Check OFFSET and LIMIT individually:
    compileSelect(json5("{LIMIT:10}"));
    CHECK(run().size() == 10);
    compileSelect(json5("{OFFSET:90}"));
    CHECK(run().size() == 10);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query LIKE", "[Query][C]") {
    SECTION("General") {
        compile(json5("['LIKE', ['.name.first'], '%j%']"));
        CHECK(run() == (vector<string>{ "0000085" }));
        compile(json5("['LIKE', ['.name.first'], '%J%']"));
        CHECK(run() == (vector<string>{ "0000002", "0000004", "0000008", "0000017", "0000028", "0000030", "0000045", "0000052", "0000067", "0000071",
            "0000088", "0000094" }));
        compile(json5("['LIKE', ['.name.first'], 'Jen%']"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));

        compile(json5("['LIKE', ['.name.first'], 'Jen_']"));
        CHECK(run() == (vector<string>{ "0000028" }));

        compile(json5("['LIKE', ['.name.first'], '_ene']"));
        CHECK(run() == (vector<string>{ "0000028" }));

        compile(json5("['LIKE', ['.name.first'], 'J_ne']"));
        CHECK(run() == (vector<string>{ "0000028" }));

        // Check backtracking (e.g. Janette should not fail because of extra characters
        // after Jane because there is another e at the end)
        compile(json5("['LIKE', ['.name.first'], 'J%e']"));
        CHECK(run() == (vector<string>{ "0000028", "0000052", "0000088" }));
    }

    SECTION("Escaped") {
        addPersonInState("weird", "NY", "Bart%Simpson");
        addPersonInState("weirder", "NY", "Bart\\\\Simpson");
        addPersonInState("coder", "CA", "Bart_Simpson");
        compile(json5("['LIKE', ['.name.first'], 'Bart\\\\%%']"));
        CHECK(run() == (vector<string>{ "weird" }));
        compile(json5("['LIKE', ['.name.first'], 'Bart\\\\\\\\%']"));
        CHECK(run() == (vector<string>{ "weirder" }));
        compile(json5("['LIKE', ['.name.first'], 'Bart\\\\_Simpson']"));
        CHECK(run() == (vector<string>{ "coder" }));
    }

    SECTION("Collated Case-Insensitive") {
        compile(json5(u8"['COLLATE', {'unicode': true, 'case': false, 'diac': true}, ['LIKE', ['.name.first'], 'jen%']]"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));

        compile(json5(u8"['COLLATE', {'unicode': true, 'case': false, 'diac': true}, ['LIKE', ['.name.first'], 'jén%']]"));
        CHECK(run().empty());
    }

    SECTION("Collated Diacritic-Insensitive") {
        compile(json5(u8"['COLLATE', {'unicode': true, 'case': true, 'diac': false}, ['LIKE', ['.name.first'], 'Jén%']]"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));

        compile(json5(u8"['COLLATE', {'unicode': true, 'case': true, 'diac': false}, ['LIKE', ['.name.first'], 'jén%']]"));
        CHECK(run().empty());
    }

    SECTION("Everything insensitive") {
        compile(json5(u8"['COLLATE', {'unicode': true, 'case': false, 'diac': false}, ['LIKE', ['.name.first'], 'jén%']]"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));
    }
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query Contains", "[Query][C]") {
    SECTION("General") {
        compile(json5("['CONTAINS()', ['.name.first'], 'Jen']"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));

        compile(json5("['CONTAINS()', ['.name.first'], 'jen']"));
        CHECK(run().empty());

        compile(json5("['CONTAINS()', ['.name.first'], 'Jén']"));
        CHECK(run().empty());
    }

    SECTION("Collated Case-Insensitive") {
        compile(json5(u8"['COLLATE', {'unicode': true, 'case': false, 'diac': true}, ['CONTAINS()', ['.name.first'], 'jen']]"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));

        compile(json5(u8"['COLLATE', {'unicode': true, 'case': false, 'diac': true}, ['CONTAINS()', ['.name.first'], 'jén']]"));
        CHECK(run().empty());
    }

    SECTION("Collated Diacritic-Insensitive") {
        compile(json5(u8"['COLLATE', {'unicode': true, 'case': true, 'diac': false}, ['CONTAINS()', ['.name.first'], 'Jén']]"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));

        compile(json5(u8"['COLLATE', {'unicode': true, 'case': true, 'diac': false}, ['CONTAINS()', ['.name.first'], 'jén']]"));
        CHECK(run().empty());
    }

    SECTION("Everything insensitive") {
        compile(json5(u8"['COLLATE', {'unicode': true, 'case': false, 'diac': false}, ['CONTAINS()', ['.name.first'], 'jén']]"));
        CHECK(run() == (vector<string>{ "0000008", "0000028" }));
    }
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query IN", "[Query][C]") {
    // Type 1: RHS is an expression; generates a call to array_contains
    compile(json5("['IN', 'reading', ['.', 'likes']]"));
    CHECK(run() == (vector<string>{"0000004", "0000056", "0000064", "0000079", "0000099"}));

    // Type 2: RHS is an array literal; generates a SQL "IN" expression
    compile(json5("['IN', ['.', 'name', 'first'], ['[]', 'Eddie', 'Verna']]"));
    CHECK(run() == (vector<string>{"0000091", "0000093"}));
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query sorted", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"),
            json5("[['.', 'name', 'last']]"));
    CHECK(run() == (vector<string>{"0000015", "0000036", "0000072", "0000043", "0000001", "0000064", "0000073", "0000053"}));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query bindings", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 1]]"));
    CHECK(run("{\"1\": \"CA\"}") == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
    compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 'state']]"));
    CHECK(run("{\"state\": \"CA\"}") == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
}


// Check binding arrays and dicts
N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query binding types", "[Query][C]") {
    vector<string> queries = {
        "['$param']",
        "['_.', {foo: ['$param']}, 'foo']",
        "['_.', ['[]', 1, ['$param'], 3], '[1]']",
    };
    for (string what : queries) {
        C4Log("---- %s ----", what.c_str());
        compileSelect(json5("{WHAT: [" + what + "], LIMIT: 1}"));
        CHECK(run("{\"param\": 177}") == (vector<string>{"177"}));
        CHECK(run("{\"param\": \"foo\"}") == (vector<string>{"foo"}));
        CHECK(run("{\"param\": [1, 2, [3, 4]]}") == (vector<string>{"[1,2,[3,4]]"}));
        CHECK(run("{\"param\": {\"foo\": 17}}") == (vector<string>{"{\"foo\":17}"}));

        // bind a blob:
        Encoder enc;
        enc.beginDict();
        enc.writeKey("param"_sl);
        enc.writeData("\001\002\003\004\005"_sl);
        enc.endDict();
        auto binding = enc.finish();
        CHECK(run(string(binding).c_str()) == (vector<string>{"\"AQIDBAU=\""})); // (base64 encoding)
    }

    compileSelect(json5("{WHAT: [['array_count()', ['$param']]], LIMIT: 1}"));
    CHECK(run("{\"param\": [1, 2, [3, 4]]}") == (vector<string>{"3"}));
    compileSelect(json5("{WHAT: [['_.', ['$param'], 'foo']], LIMIT: 1}"));
    CHECK(run("{\"param\": {\"foo\": 17}}") == (vector<string>{"17"}));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query ANY", "[Query][C]") {
    compile(json5("['ANY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'climbing']]"));
    CHECK(run() == (vector<string>{"0000017", "0000021", "0000023", "0000045", "0000060"}));

    // This EVERY query has lots of results because every empty `likes` array matches it
    compile(json5("['EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
    auto result = run();
    REQUIRE(result.size() == 42);
    CHECK(result[0] == "0000007");

    // Changing the op to ANY AND EVERY returns no results
    compile(json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
    CHECK(run() == (vector<string>{}));

    // Look for people where everything they like contains an L:
    compile(json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['LIKE', ['?', 'like'], '%l%']]"));
    CHECK(run() == (vector<string>{ "0000017", "0000027", "0000060", "0000068" }));
}


N_WAY_TEST_CASE_METHOD(PathsQueryTest, "C4Query ANY w/paths", "[Query][C]") {
    // For https://github.com/couchbase/couchbase-lite-core/issues/238
    compile(json5("['ANY','path',['.paths'],['=',['?path','city'],'San Jose']]"));
    CHECK(run() == (vector<string>{ "0000001" }));

    compile(json5("['ANY','path',['.paths'],['=',['?path.city'],'San Jose']]"));
    CHECK(run() == (vector<string>{ "0000001" }));

    compile(json5("['ANY','path',['.paths'],['=',['?path','city'],'Palo Alto']]"));
    CHECK(run() == (vector<string>{ "0000001", "0000002" }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query ANY of dict", "[Query][C]") {
    compile(json5("['ANY', 'n', ['.', 'name'], ['=', ['?', 'n'], 'Arturo']]"));
    CHECK(run() == (vector<string>{"0000090"}));
    compile(json5("['ANY', 'n', ['.', 'name'], ['contains()', ['?', 'n'], 'V']]"));
    CHECK(run() == (vector<string>{ "0000044", "0000048", "0000053", "0000093" }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query Nested ANY of dict", "[Query][C]") {     // CBL-1248
    C4Error error;
    TransactionHelper t(db);

    // New Doc:
    auto doc1 = c4doc_create(db, C4STR("doc1"),
                             json2fleece("{'variants': [{'items': [{'id':1, 'value': 1}]}]}"), 0, ERROR_INFO(error));

    auto doc2 = c4doc_create(db, C4STR("doc2"),
                             json2fleece("{'variants': [{'items': [{'id':2, 'value': 2}]}]}"), 0, ERROR_INFO(error));

    compile(json5("['ANY', 'V', ['.variants'], ['ANY', 'I', ['?V.items'], ['=', ['?I.id'], 2]]]"));

    CHECK(run() == (vector<string>{ "doc2" }));

    c4doc_release(doc1);
    c4doc_release(doc2);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query expression index", "[Query][C]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("length"), c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, WITH_ERROR(&err)));
    compile(json5("['=', ['length()', ['.name.first']], 9]"));
    CHECK(run() == (vector<string>{ "0000015", "0000099" }));

}


static bool lookForIndex(C4Database *db, slice name) {
    bool found = false;
    Doc info(alloc_slice(c4db_getIndexesInfo(db, nullptr)));
    for (Array::iterator i(info.asArray()); i; ++i) {
        Dict index = i.value().asDict();
        C4Log("-- index: %s", index.toJSONString().c_str());
        if (index["name"].asString() == name)
            found = true;
    }
    return found;
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "Reindex", "[Query][C]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("length"), c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, WITH_ERROR(&err)));
    CHECK(lookForIndex(db, "length"_sl));
    compile(json5("['=', ['length()', ['.name.first']], 9]"));
    CHECK(run() == (vector<string>{ "0000015", "0000099" }));

    C4Log("Reindexing");
    REQUIRE(c4db_maintenance(db, kC4Reindex, WITH_ERROR(&err)));

    CHECK(lookForIndex(db, "length"_sl));
    CHECK(run() == (vector<string>{ "0000015", "0000099" }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "Delete indexed doc", "[Query][C]") {
    // Create the same index as the above test:
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("length"), c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, WITH_ERROR(&err)));

    // Delete doc "0000015":
    {
        TransactionHelper t(db);

        C4Error c4err;
        C4Document *doc = c4doc_get(db, C4STR("0000015"), true, ERROR_INFO(&c4err));
        REQUIRE(doc);
        C4DocPutRequest rq = {};
        rq.docID = C4STR("0000015");
        rq.history = &doc->revID;
        rq.historyCount = 1;
        rq.revFlags = kRevDeleted;
        rq.save = true;
        C4Document *updatedDoc = c4doc_put(db, &rq, nullptr, ERROR_INFO(&c4err));
        REQUIRE(updatedDoc != nullptr);
        c4doc_release(doc);
        c4doc_release(updatedDoc);
    }

    // Now run a query that would have returned the deleted doc, if it weren't deleted:
    compile(json5("['=', ['length()', ['.name.first']], 9]"));
    CHECK(run() == (vector<string>{ "0000099" }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "Column titles", "[Query][C]") {
    // Properties:
    compileSelect(json5("['SELECT', {'WHAT': [['.'], ['.name'], '.gender', ['.', 'address', 'zip']]}]"));
    checkColumnTitles({"_doc", "name", "gender", "zip"});
    // Duplicates:
    compileSelect(json5("['SELECT', {'WHAT': ['.name', '.name', '.name']}]"));
    checkColumnTitles({"name", "name #2", "name #3"});
    // 'AS':
    compileSelect(json5("['SELECT', {'WHAT': [['AS', '.address.zip', 'ZIP']]}]"));
    checkColumnTitles({"ZIP"});
    // Expressions:
    compileSelect(json5("['SELECT', {'WHAT': [['+', ['.age'], 14], ['min()', ['.n']]]}]"));
    checkColumnTitles({"$1", "$2"});
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "Missing columns", "[Query][C]") {
    const char *query = nullptr;
    uint64_t expectedMissing = 0;
    SECTION("None missing1") {
        query = "['SELECT', {'WHAT': [['.name'], ['.gender']], 'LIMIT': 1}]";
        expectedMissing = 0x0;
    }
    SECTION("Some missing2") {
        query = "['SELECT', {'WHAT': [['.XX'], ['.name'], ['.YY'], ['.gender'], ['.ZZ']], 'LIMIT': 1}]";
        expectedMissing = 0x15;       // binary 10101, i.e. cols 0, 2, 4 are missing
    }
    if (query) {
        compileSelect(json5(query));
        auto results = runCollecting<uint64_t>(nullptr, [=](C4QueryEnumerator *e) {
            return e->missingColumns;
        });
        CHECK(results == vector<uint64_t>{expectedMissing});
    }
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "Blob access", "[Query][Blob][C]") {
    string blob = "This is a blob to store in the store!";
    vector<C4BlobKey> keys;
    {
        TransactionHelper t(db);
        keys = addDocWithAttachments("doc1"_sl,
                                     vector<string>{blob},
                                     "text/plain");
    }
    compileSelect(json5("['SELECT', {WHAT: [['BLOB', '.attached[0]']], WHERE: ['=', ['._id'], 'doc1']}]"));
    auto results = runCollecting<string>(nullptr, [=](C4QueryEnumerator *e) {
        return string(slice(FLValue_AsData(FLArrayIterator_GetValueAt(&e->columns, 0))));
    });
    CHECK(results == vector<string>{blob});

    // Same as above, but wrap the blob in an array when returning it from the query:
    compileSelect(json5("['SELECT', {WHAT: [['[]', ['BLOB', '.attached[0]']]], WHERE: ['=', ['._id'], 'doc1']}]"));
    results = runCollecting<string>(nullptr, [=](C4QueryEnumerator *e) {
        FLValue result = FLArrayIterator_GetValueAt(&e->columns, 0);
        FLValue item = FLArray_Get(FLValue_AsArray(result), 0);
        return string(slice(FLValue_AsData(item)));
    });
    CHECK(results == vector<string>{blob});
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query dict literal", "[Query][C]") {
    compileSelect(json5("{WHAT: [{n: null, f: false, t: true, i: 12345, d: 1234.5, s: 'howdy', m: ['.bogus'], id: ['._id']}]}"));

    auto results = runCollecting<string>(nullptr, [=](C4QueryEnumerator *e) {
        FLValue result = FLArrayIterator_GetValueAt(&e->columns, 0);
        return string(alloc_slice(FLValue_ToJSON5(result)));
    });
    CHECK(results[0] == "{d:1234.5,f:false,i:12345,id:\"0000001\",n:null,s:\"howdy\",t:true}");
}

#pragma mark - FTS:


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    compile(json5("['MATCH()', 'byStreet', 'Hwy']"));
    auto results = runFTS();
    CHECK(results == (vector<vector<C4FullTextMatch>>{
        {{13, 0, 0, 10, 3}},
        {{15, 0, 0, 11, 3}},
        {{43, 0, 0, 12, 3}},
        {{44, 0, 0, 12, 3}},
        {{52, 0, 0, 11, 3}}
    }));
    
    C4SliceResult matched = c4query_fullTextMatched(query, &results[0][0], ERROR_INFO(err));
    REQUIRE(matched.buf != nullptr);
    CHECK(toString(matched) == "7 Wyoming Hwy");
    c4slice_free(matched);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS multiple properties", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byAddress"),
                             C4STR("[[\".contact.address.street\"], [\".contact.address.city\"], [\".contact.address.state\"]]"), kC4FullTextIndex, nullptr, ERROR_INFO(err)));
    // Some docs match 'Santa' in the street name, some in the city name
    compile(json5("['MATCH()', 'byAddress', 'Santa']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {15, 1, 0, 0, 5} },
        { {44, 0, 0, 3, 5} },
        { {68, 0, 0, 3, 5} },
        { {72, 1, 0, 0, 5} },
    }));

    // Search only the street name:
    compile(json5("['MATCH()', 'byAddress', 'contact.address.street:Santa']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {44, 0, 0, 3, 5} },
        { {68, 0, 0, 3, 5} }
    }));

    // Search for 'Santa' in the street name, and 'Saint' in either:
    compile(json5("['MATCH()', 'byAddress', 'contact.address.street:Santa Saint']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {68, 0, 0, 3, 5}, {68, 1, 1, 0, 5} }
    }));

    // Search for 'Santa' in the street name, _or_ 'Saint' in either:
    compile(json5("['MATCH()', 'byAddress', 'contact.address.street:Santa OR Saint']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {20, 1, 1, 0, 5} },
        { {44, 0, 0, 3, 5} },
        { {68, 0, 0, 3, 5}, {68, 1, 1, 0, 5} },
        { {77, 1, 1, 0, 5} }
    }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS Multiple indexes", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    REQUIRE(c4db_createIndex(db, C4STR("byCity"), C4STR("[[\".contact.address.city\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    compile(json5("['AND', ['MATCH()', 'byStreet', 'Hwy'],\
                           ['MATCH()', 'byCity',   'Santa']]"));
    CHECK(run() == (vector<string>{"0000015"}));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {15, 0, 0, 11, 3} }
    }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS multiple ANDs", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    REQUIRE(c4db_createIndex(db, C4STR("byCity"), C4STR("[[\".contact.address.city\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    compile(json5("['AND', ['AND', ['=', ['.gender'], 'male'],\
                                   ['MATCH()', 'byCity', 'Santa']],\
                           ['=', ['.name.first'], 'Cleveland']]"));
    CHECK(run() == (vector<string>{"0000015"}));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {15, 0, 0, 0, 5} }
    }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS Multiple queries", "[Query][C][FTS][!throws]") {
    // You can't query the same FTS index multiple times in a query (says SQLite)
    ExpectingExceptions x;
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    query = c4query_new2(db, kC4JSONQuery,
                        json5slice("['AND', ['MATCH()', 'byStreet', 'Hwy'],\
                                            ['MATCH()', 'byStreet', 'Blvd']]"),
                        nullptr, &err);
    REQUIRE(query == nullptr);
    CheckError(err, LiteCoreDomain, kC4ErrorInvalidQuery,
               "Sorry, multiple MATCHes of the same property are not allowed");
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS Buried", "[Query][C][FTS][!throws]") {
    // You can't put an FTS match inside an expression other than a top-level AND (says SQLite)
    ExpectingExceptions x;
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    query = c4query_new2(db, kC4JSONQuery,
                        json5slice("['OR', ['MATCH()', 'byStreet', 'Hwy'],\
                                           ['=', ['.', 'contact', 'address', 'state'], 'CA']]"),
                         nullptr, &err);
    REQUIRE(query == nullptr);
    CheckError(err, LiteCoreDomain, kC4ErrorInvalidQuery,
               "MATCH can only appear at top-level, or in a top-level AND");
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS Aggregate", "[Query][C][FTS]") {
    // https://github.com/couchbase/couchbase-lite-core/issues/703
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    query = c4query_new2(db, kC4JSONQuery,
            json5slice("['SELECT', { 'WHAT': [ [ 'count()', [ '.', 'uuid' ] ] ],"
                       " 'WHERE': [ 'AND', [ 'AND', [ '=', [ '.', 'doc_type' ], 'rec' ],"
                                                  " [ 'MATCH()', 'byStreet', 'keyword' ] ],"
                                         "[ '=', [ '.', 'pId' ], 'bfe2970b-9be6-46f6-b9a7-38c5947c27b1' ] ] } ]"),
                         nullptr, ERROR_INFO(err));
    REQUIRE(query);
    // Just test whether the enumerator starts without an error:
    auto e = c4query_run(query, nullptr, nullslice, ERROR_INFO(err));
    REQUIRE(e);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS with alias", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, WITH_ERROR(&err)));
    query = c4query_new2(db, kC4JSONQuery,
            json5slice("['SELECT', { 'WHAT': [ [ '.db.uuid' ] ],"
                       " 'FROM': [{ 'AS' : 'db'}],"
                       " 'WHERE': [ 'AND', [ 'AND', [ '=', [ '.db.doc_type' ], 'rec' ],"
                                                  " [ 'MATCH()', 'db.byStreet', 'keyword' ] ],"
                                         "[ '=', [ '.db.pId' ], 'bfe2970b-9be6-46f6-b9a7-38c5947c27b1' ] ] } ]"),
                         nullptr, ERROR_INFO(err));
    REQUIRE(query);
    // Just test whether the enumerator starts without an error:
    auto e = c4query_run(query, nullptr, nullslice, ERROR_INFO(err));
    REQUIRE(e);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS with accents", "[Query][C][FTS]") {
    // https://github.com/couchbase/couchbase-lite-core/issues/723
    C4Error err;
    C4IndexOptions options = {
       nullptr, false, false, nullptr
    };

    REQUIRE(c4db_createIndex(db, C4STR("nameFTSIndex"), C4STR("[[\".content\"]]"), kC4FullTextIndex, &options, WITH_ERROR(&err)));

    {
        TransactionHelper t(db);

        C4SliceResult bodyContent = c4db_encodeJSON(db, C4STR(u8"{\"content\": \"Hâkimler\"}"), ERROR_INFO(err));
        REQUIRE(bodyContent.buf != nullptr);
        createNewRev(db, C4STR("1"), (C4Slice)bodyContent);
        c4slice_free(bodyContent);

        bodyContent = c4db_encodeJSON(db, C4STR("{\"content\": \"Hakimler\"}"), ERROR_INFO(err));
        REQUIRE(bodyContent.buf != nullptr);
        createNewRev(db, C4STR("2"), (C4Slice)bodyContent);
        c4slice_free(bodyContent);

        bodyContent = c4db_encodeJSON(db, C4STR("{\"content\": \"foo\"}"), ERROR_INFO(err));
        REQUIRE(bodyContent.buf != nullptr);
        createNewRev(db, C4STR("3"), (C4Slice)bodyContent);
        c4slice_free(bodyContent);
    }

    C4Slice queryStr = C4STR("{\"WHERE\": [\"MATCH()\",\"nameFTSIndex\",\"'hâkimler'\"], \"WHAT\": [[\".\"]]}");
    query = c4query_new2(db, kC4JSONQuery, queryStr, nullptr, ERROR_INFO(err));
    REQUIRE(query);
    auto e = c4query_run(query, nullptr, nullslice, ERROR_INFO(err));
    REQUIRE(e);
    CHECK(c4queryenum_getRowCount(e, WITH_ERROR(&err)) == 1);
    c4queryenum_release(e);
}


#pragma mark - WHAT, JOIN, etc:


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query WHAT", "[Query][C]") {
    vector<string> expectedFirst = {"Cleveland", "Georgetta", "Margaretta"};
    vector<string> expectedLast  = {"Bejcek",    "Kolding",   "Ogwynn"};
    compileSelect(json5("{WHAT: ['.name.first', '.name.last'], \
                         WHERE: ['>=', ['length()', ['.name.first']], 9],\
                      ORDER_BY: [['.name.first']]}"));

    REQUIRE(c4query_columnCount(query) == 2);

    C4Error error;
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, ERROR_INFO(error))) {
        CHECK(Array::iterator(e->columns)[0].asstring() == expectedFirst[i]);
        CHECK(Array::iterator(e->columns)[1].asstring() == expectedLast[i]);
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == 3);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query WHAT returning object", "[Query][C]") {
    vector<string> expectedFirst = {"Cleveland", "Georgetta", "Margaretta"};
    vector<string> expectedLast  = {"Bejcek",    "Kolding",   "Ogwynn"};
    compileSelect(json5("{WHAT: ['.name'], \
                         WHERE: ['>=', ['length()', ['.name.first']], 9],\
                      ORDER_BY: [['.name.first']]}"));

    REQUIRE(c4query_columnCount(query) == 1);

    C4Error error;
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, ERROR_INFO(error))) {
        Value col = Array::iterator(e->columns)[0];
        REQUIRE(col.type() == kFLDict);
        Dict name = col.asDict();
        INFO("name = " << FLSlice(name.toJSON()));
        CHECK(name.get("first"_sl).asstring() == expectedFirst[i]);
        CHECK(name.get("last"_sl) .asstring() == expectedLast[i]);
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == 3);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query Aggregate", "[Query][C]") {
    compileSelect(json5("{WHAT: [['min()', ['.name.last']], ['max()', ['.name.last']]]}"));
    C4Error error;
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, ERROR_INFO(error))) {
        CHECK(Array::iterator(e->columns)[0].asstring() == "Aerni");
        CHECK(Array::iterator(e->columns)[1].asstring() == "Zirk");
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == 1);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query Grouped", "[Query][C]") {
    const vector<string> expectedState = {"AL",      "AR",        "AZ",       "CA"};
    const vector<string> expectedMin   = {"Laidlaw", "Okorududu", "Kinatyan", "Bejcek"};
    const vector<string> expectedMax   = {"Mulneix", "Schmith",   "Kinatyan", "Visnic"};
    const int expectedRowCount = 42;

    compileSelect(json5("{WHAT: [['.contact.address.state'],\
                                ['min()', ['.name.last']],\
                                ['max()', ['.name.last']]],\
                     GROUP_BY: [['.contact.address.state']]}"));
    C4Error error {};
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, ERROR_INFO(error))) {
        string state   = Array::iterator(e->columns)[0].asstring();
        string minName = Array::iterator(e->columns)[1].asstring();
        string maxName = Array::iterator(e->columns)[2].asstring();
        C4Log("state=%s, first=%s, last=%s", state.c_str(), minName.c_str(), maxName.c_str());
        if (i < expectedState.size()) {
            CHECK(state == expectedState[i]);
            CHECK(minName == expectedMin[i]);
            CHECK(maxName == expectedMax[i]);
        }
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == expectedRowCount);
    CHECK(c4queryenum_getRowCount(e, WITH_ERROR(&error)) == 42);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query Join", "[Query][C]") {
    importJSONFile(sFixturesDir + "states_titlecase.json", "state-");
    vector<string> expectedFirst = {"Cleveland",   "Georgetta", "Margaretta"};
    vector<string> expectedState  = {"California", "Ohio",      "South Dakota"};
    compileSelect(json5("{WHAT: ['.person.name.first', '.state.name'],\
                          FROM: [{as: 'person'}, \
                                 {as: 'state', on: ['=', ['.state.abbreviation'],\
                                                         ['.person.contact.address.state']]}],\
                         WHERE: ['>=', ['length()', ['.person.name.first']], 9],\
                      ORDER_BY: [['.person.name.first']]}"));
    C4Error error;
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, ERROR_INFO(error))) {
        string first = Array::iterator(e->columns)[0].asstring();
        string state = Array::iterator(e->columns)[1].asstring();
        C4Log("first='%s', state='%s'", first.c_str(), state.c_str());
        CHECK(first == expectedFirst[i]);
        CHECK(state  == expectedState[i]);
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == 3);
    c4queryenum_release(e);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query UNNEST", "[Query][C]") {
    for (int withIndex = 0; withIndex <= 1; ++withIndex) {
        if (withIndex) {
            C4Log("-------- Repeating with index --------");
            REQUIRE(c4db_createIndex(db, C4STR("likes"), C4STR("[[\".likes\"]]"), kC4ArrayIndex, nullptr, nullptr));
        }
        compileSelect(json5("{WHAT: ['.person._id'],\
                              FROM: [{as: 'person'}, \
                                     {as: 'like', unnest: ['.person.likes']}],\
                             WHERE: ['=', ['.like'], 'climbing'],\
                          ORDER_BY: [['.person.name.first']]}"));
        checkExplanation(withIndex);
        CHECK(run() == (vector<string>{ "0000021", "0000017", "0000045", "0000060", "0000023" }));

        compileSelect(json5("{WHAT: ['.person._id', '.like'],\
                              FROM: [{as: 'person'}, \
                                     {as: 'like', unnest: ['.person.likes']}],\
                             WHERE: ['>', ['.like'], 'snowboarding'],\
                          ORDER_BY: [['.like'], ['.person._id']]}"));
        checkExplanation(withIndex);
        CHECK(run2() == (vector<string>{ "0000003, swimming", "0000012, swimming", "0000020, swimming",
            "0000072, swimming", "0000076, swimming", "0000081, swimming", "0000085, swimming",
            "0000010, travelling", "0000027, travelling", "0000037, travelling", "0000060, travelling",
            "0000068, travelling", "0000096, travelling" }));

        compileSelect(json5("{WHAT: ['.like'],\
                          DISTINCT: true,\
                              FROM: [{as: 'person'}, \
                                     {as: 'like', unnest: ['.person.likes']}],\
                          ORDER_BY: [['.like']]}"));
        checkExplanation(false);        // even with index, this must do a scan
        CHECK(run() == (vector<string>{ "biking", "boxing", "chatting", "checkers", "chess", "climbing", "driving", "ironing", "reading", "running",
                                        "shopping", "skiing", "snowboarding", "swimming", "travelling" }));
    }
}

N_WAY_TEST_CASE_METHOD(NestedQueryTest, "C4Query UNNEST objects", "[Query][C]") {
    for (int withIndex = 0; withIndex <= 1; ++withIndex) {
        if (withIndex) {
            C4Log("-------- Repeating with index --------");
            REQUIRE(c4db_createIndex(db, C4STR("shapes"), C4STR("[[\".shapes\"], [\".color\"]]"), kC4ArrayIndex, nullptr, nullptr));
        }
        compileSelect(json5("{WHAT: ['.shape.color'],\
                          DISTINCT: true,\
                              FROM: [{as: 'doc'}, \
                                     {as: 'shape', unnest: ['.doc.shapes']}],\
                          ORDER_BY: [['.shape.color']]}"));
        checkExplanation(false);        // even with index, this must do a scan
        CHECK(run() == (vector<string>{ "blue", "cyan", "green", "red", "white", "yellow" }));

        compileSelect(json5("{WHAT: [['sum()', ['.shape.size']]],\
                              FROM: [{as: 'doc'}, \
                                     {as: 'shape', unnest: ['.doc.shapes']}]}"));
        checkExplanation(false);        // even with index, this must do a scan
        CHECK(run() == (vector<string>{ "32" }));

        compileSelect(json5("{WHAT: [['sum()', ['.shape.size']]],\
                              FROM: [{as: 'doc'}, \
                                     {as: 'shape', unnest: ['.doc.shapes']}],\
                             WHERE: ['=', ['.shape.color'], 'red']}"));
        checkExplanation(withIndex);
        CHECK(run() == (vector<string>{ "11" }));
    }
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query Seek", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    C4Error error;
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
    REQUIRE(e);
    REQUIRE(c4queryenum_next(e, WITH_ERROR(&error)));
    REQUIRE(FLArrayIterator_GetCount(&e->columns) > 0);
    FLString docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
    REQUIRE(docID == "0000001"_sl);
    REQUIRE(c4queryenum_next(e, WITH_ERROR(&error)));
    REQUIRE(c4queryenum_seek(e, 0, WITH_ERROR(&error)));
    docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
    REQUIRE(docID == "0000001"_sl);
    REQUIRE(c4queryenum_seek(e, 7, WITH_ERROR(&error)));
    docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
    REQUIRE(docID == "0000073"_sl);
    {
        ExpectingExceptions ex;
        REQUIRE(!c4queryenum_seek(e, 100, &error));
    }
    
    CHECK(error == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(NestedQueryTest, "C4Query ANY nested", "[Query][C]") {
    compile(json5("['ANY', 'Shape', ['.', 'shapes'], ['=', ['?', 'Shape', 'color'], 'red']]"));
    CHECK(run() == (vector<string>{"0000001", "0000003"}));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query parser error messages", "[Query][C][!throws]") {
    ExpectingExceptions x;

    C4Error error;
    query = c4query_new2(db, kC4JSONQuery, c4str("[\"=\"]"), nullptr, &error);
    REQUIRE(query == nullptr);
    CheckError(error, LiteCoreDomain, kC4ErrorInvalidQuery, "Wrong number of arguments to =");
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query refresh", "[Query][C][!throws]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    C4Error error;
    
    string explanationString = toString(c4query_explain(query));
    CHECK(litecore::hasPrefix(explanationString, "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'contact.address.state') = 'CA') AND (_doc.flags & 1 = 0)"));
    
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
    REQUIRE(e);
    auto refreshed = c4queryenum_refresh(e, ERROR_INFO(error));
    REQUIRE(!refreshed);
    
    addPersonInState("added_later", "CA");
    
    refreshed = c4queryenum_refresh(e, ERROR_INFO(error));
    REQUIRE(refreshed);
    auto count = c4queryenum_getRowCount(refreshed, ERROR_INFO(error));
    REQUIRE(count > 0);
    REQUIRE(c4queryenum_seek(refreshed, count - 1, WITH_ERROR(&error)));
    CHECK(FLValue_AsString(FLArrayIterator_GetValueAt(&refreshed->columns, 0)) == "added_later"_sl);
    c4queryenum_release(refreshed);

    {
        TransactionHelper t(db);
        REQUIRE(c4db_purgeDoc(db, "added_later"_sl, WITH_ERROR(&error)));
    }

    refreshed = c4queryenum_refresh(e, ERROR_INFO(error));
    REQUIRE(refreshed);
    c4queryenum_close(e);
    count = c4queryenum_getRowCount(refreshed, ERROR_INFO(error));
    REQUIRE(count > 0);
    REQUIRE(c4queryenum_seek(refreshed, count - 1, WITH_ERROR(&error)));
    CHECK(FLValue_AsString(FLArrayIterator_GetValueAt(&refreshed->columns, 0)) != "added_later"_sl);

    c4queryenum_release(e);
    c4queryenum_release(refreshed);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query observer", "[Query][C][!throws]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    C4Error error;

    struct State {
        C4Query *query;
        c4::ref<C4QueryObserver> obs;
        atomic<int> count = 0;
    };

    auto callback = [](C4QueryObserver *obs, C4Query *query, void *context) {
        C4Log("---- Query observer called!");
        auto state = (State*)context;
        CHECK(query == state->query);
        CHECK(obs == state->obs);
        CHECK(state->count == 0);
        ++state->count;
    };
    State state;
    state.query = query;
    state.obs = c4queryobs_create(query, callback, &state);
    CHECK(state.obs);
    c4queryobs_setEnabled(state.obs, true);

    C4Log("---- Waiting for query observer...");
    REQUIRE_BEFORE(2000ms, state.count > 0);

    C4Log("Checking query observer...");
    CHECK(state.count == 1);
    c4::ref<C4QueryEnumerator> e = c4queryobs_getEnumerator(state.obs, true, ERROR_INFO(error));
    REQUIRE(e);
    CHECK(c4queryobs_getEnumerator(state.obs, true, &error) == nullptr);
    CHECK(error.code == 0);
    CHECK(c4queryenum_getRowCount(e, WITH_ERROR(&error)) == 8);
    state.count = 0;

    addPersonInState("after1", "AL");

    C4Log("---- Checking that query observer doesn't fire...");
    this_thread::sleep_for(1000ms);
    REQUIRE(state.count == 0);

    {
        C4Log("---- Changing a doc in the query");
        TransactionHelper t(db);
        addPersonInState("after2", "CA");
        // wait, to make sure the observer doesn't try to run the query before the commit
        this_thread::sleep_for(1000ms);
        C4Log("---- Commiting changes");
    }

    C4Log("---- Waiting for 2nd call of query observer...");
    REQUIRE_BEFORE(2000ms, state.count > 0);

    C4Log("---- Checking query observer again...");
    CHECK(state.count == 1);
    c4::ref<C4QueryEnumerator> e2 = c4queryobs_getEnumerator(state.obs, false, ERROR_INFO(error));
    REQUIRE(e2);
    CHECK(e2 != e);
    c4::ref<C4QueryEnumerator> e3 = c4queryobs_getEnumerator(state.obs, false, ERROR_INFO(error));
    CHECK(e3 == e2);
    CHECK(c4queryenum_getRowCount(e2, WITH_ERROR(&error)) == 9);

    // Testing with purged document:
    C4Log("---- Purging a document...");
    state.count = 0;
    {
        TransactionHelper t(db);
        REQUIRE(c4db_purgeDoc(db, "after2"_sl, WITH_ERROR(&error)));
        C4Log("---- Commiting changes");
    }

    C4Log("---- Waiting for 3rd call of query observer...");
    REQUIRE_BEFORE(2000ms, state.count > 0);

    C4Log("---- Checking query observer again...");
    CHECK(state.count == 1);
    e2 = c4queryobs_getEnumerator(state.obs, true, ERROR_INFO(error));
    REQUIRE(e2);
    CHECK(e2 != e);
    CHECK(c4queryenum_getRowCount(e2, WITH_ERROR(&error)) == 8);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query observer with changing query parameters", "[Query][C][!throws]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$state']]"));
    C4Error error;

    struct State {
        C4Query *query;
        c4::ref<C4QueryObserver> obs;
        atomic<int> count = 0;
    };

    auto callback = [](C4QueryObserver *obs, C4Query *query, void *context) {
        C4Log("---- Query observer called!");
        auto state = (State*)context;
        CHECK(query == state->query);
        CHECK(obs == state->obs);
        CHECK(state->count == 0);
        ++state->count;
    };
    
    Encoder enc;
    enc.beginDict();
    enc.writeKey("state"_sl);
    enc.writeString("CA");
    enc.endDict();
    auto params = enc.finish();
    c4query_setParameters(query, params);
    
    auto explain = c4query_explain(query);
    CHECK(explain);
    C4Log("Explain = %.*s", (int)explain.size, (char*)explain.buf);
    
    State state;
    state.query = query;
    state.obs = c4queryobs_create(query, callback, &state);
    CHECK(state.obs);
    c4queryobs_setEnabled(state.obs, true);

    C4Log("---- Waiting for query observers...");
    REQUIRE_BEFORE(2000ms, state.count > 0);

    C4Log("Checking query observers...");
    CHECK(state.count == 1);
    c4::ref<C4QueryEnumerator> e1 = c4queryobs_getEnumerator(state.obs, true, ERROR_INFO(error));
    REQUIRE(e1);
    CHECK(error.code == 0);
    CHECK(c4queryenum_getRowCount(e1, WITH_ERROR(&error)) == 8);
    
    state.count = 0;
    enc.beginDict();
    enc.writeKey("state"_sl);
    enc.writeString("NY");
    enc.endDict();
    params = enc.finish();
    c4query_setParameters(query, params);
    
    C4Log("---- Waiting for query observers after changing the parameters...");
    REQUIRE_BEFORE(5000ms, state.count > 0);

    C4Log("Checking query observers...");
    CHECK(state.count == 1);
    c4::ref<C4QueryEnumerator> e2 = c4queryobs_getEnumerator(state.obs, true, ERROR_INFO(error));
    REQUIRE(e2);
    CHECK(error.code == 0);
    CHECK(c4queryenum_getRowCount(e2, WITH_ERROR(&error)) == 9);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "Delete index", "[Query][C][!throws]") {
    C4Error err;
    C4String names[2] = { C4STR("length"), C4STR("byStreet") };
    string desc1 = json5("[['length()', ['.name.first']]]");
    C4String desc[2] = {c4str(desc1.c_str()), C4STR("[[\".contact.address.street\"]]") };
    C4IndexType types[2] = { kC4ValueIndex, kC4FullTextIndex };
    
    for(int i = 0; i < 2; i++) {
        REQUIRE(c4db_createIndex(db, names[i], desc[i], types[i], nullptr, WITH_ERROR(&err)));

        alloc_slice indexes = c4db_getIndexesInfo(db, ERROR_INFO(err));
        REQUIRE(indexes);
        FLArray indexArray = FLValue_AsArray(FLValue_FromData(indexes, kFLTrusted));
        REQUIRE(FLArray_Count(indexArray) == 1);
        FLDict indexInfo = FLValue_AsDict(FLArray_Get(indexArray, 0));
        FLSlice indexName = FLValue_AsString(FLDict_Get(indexInfo, "name"_sl));
        CHECK(indexName == names[i]);

        REQUIRE(c4db_deleteIndex(db, names[i], WITH_ERROR(&err)));

        indexes = c4db_getIndexesInfo(db, ERROR_INFO(err));
        indexArray = FLValue_AsArray(FLValue_FromData(indexes, kFLTrusted));
        REQUIRE(FLArray_Count(indexArray) == 0);
    }
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "Database alias column names", "[Query][C][!throws]") {
    // https://github.com/couchbase/couchbase-lite-core/issues/750

    C4Error err;
    string queryText = "{'WHAT':[['.main.'],['.secondary.']],'FROM':[{'AS':'main'},{'AS':'secondary','ON':['=',['.main.number1'],['.secondary.theone']]}]}";
    FLSliceResult queryStr = FLJSON5_ToJSON({queryText.data(), queryText.size()}, nullptr, nullptr, nullptr);
    query = c4query_new2(db, kC4JSONQuery, (C4Slice)queryStr, nullptr, ERROR_INFO(err));
    REQUIRE(query);
    FLSlice expected1 = FLSTR("main");
    FLSlice expected2 = FLSTR("secondary");
    CHECK((c4query_columnTitle(query, 0) == expected1));
    CHECK((c4query_columnTitle(query, 1) == expected2));
    FLSliceResult_Release(queryStr);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query RevisionID", "[Query][C][!throws]") {
    C4Error error;
    TransactionHelper t(db);
    
    // New Doc:
    auto doc1a = c4doc_create(db, C4STR("doc1"), kC4SliceNull, 0, ERROR_INFO(error));
    REQUIRE(doc1a);
    auto revID = toString(doc1a->revID);
    compileSelect(json5("{WHAT: [['._revisionID']], WHERE: ['=', ['._id'], 'doc1']}"));
    CHECK(run() == (vector<string>{revID}));

    // Updated Doc:
    auto doc1b = c4doc_update(doc1a, json2fleece("{'ok':'go'}"), 0, ERROR_INFO(error));
    REQUIRE(doc1b);
    revID = toString(doc1b->revID);
    c4doc_release(doc1a);
    compileSelect(json5("{WHAT: [['._revisionID']], WHERE: ['=', ['._id'], 'doc1']}"));
    CHECK(run() == (vector<string>{revID}));

    // revisionID in WHERE:
    compileSelect(json5("{WHAT: [['._id']], WHERE: ['=', ['._revisionID'], '" + revID + "']}"));
    CHECK(run() == (vector<string>{"doc1"}));

    // Deleted Doc:
    auto doc1c = c4doc_update(doc1b, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
    REQUIRE(doc1c);
    revID = toString(doc1c->revID);
    c4doc_release(doc1b);
    compileSelect(json5("{WHAT: [['._revisionID']], WHERE: ['AND', ['._deleted'], ['=', ['._id'], 'doc1']]}"));
    CHECK(run() == (vector<string>{revID}));
    c4doc_release(doc1c);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query alternative FROM names", "[Query][C]") {
    TransactionHelper t(db);
    string dbName(c4db_getName(db));

    // Explicitly specify the default collection:
    compileSelect(json5("{'WHAT': ['.'], 'FROM': [{'COLLECTION':'_default'}]}"));
    CHECK(run().size() == 100);

    // Use "_" as a synonym for the default collection:
    compileSelect(json5("{'WHAT': ['.'], 'FROM': [{'COLLECTION':'_'}]}"));
    CHECK(run().size() == 100);
    compileSelect(json5("{'WHAT': ['.foo.'], 'FROM': [{'COLLECTION':'_', 'AS':'foo'}]}"));
    CHECK(run().size() == 100);

    // Use the database name as a synonym for the default collection:
    compileSelect(json5("{'WHAT': ['.'], 'FROM': [{'COLLECTION':'" + dbName + "'}]}"));
    CHECK(run().size() == 100);


    // Create a collection with the same name as the database;
    // then that name should access the new collection, not the default one:
    c4db_createCollection(db, slice(dbName), ERROR_INFO());
    compileSelect(json5("{'WHAT': ['.'], 'FROM': [{'COLLECTION':'" + dbName + "'}]}"));
    CHECK(run().size() == 0);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "Multiple C4Query observers", "[Query][C][!throws]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    C4Error error;

    struct State {
        C4Query *query;
        c4::ref<C4QueryObserver> obs;
        atomic<int> count = 0;
    };

    auto callback = [](C4QueryObserver *obs, C4Query *query, void *context) {
        C4Log("---- Query observer called!");
        auto state = (State*)context;
        CHECK(query == state->query);
        CHECK(obs == state->obs);
        CHECK(state->count == 0);
        ++state->count;
    };

    State state1;
    state1.query = query;
    state1.obs = c4queryobs_create(query, callback, &state1);
    CHECK(state1.obs);
    c4queryobs_setEnabled(state1.obs, true);

    State state2;
    state2.query = query;
    state2.obs = c4queryobs_create(query, callback, &state2);
    CHECK(state2.obs);
    c4queryobs_setEnabled(state2.obs, true);

    C4Log("---- Waiting for query observers...");
    REQUIRE_BEFORE(2000ms, state1.count > 0 && state2.count > 0);

    C4Log("Checking query observers...");
    CHECK(state1.count == 1);
    c4::ref<C4QueryEnumerator> e1 = c4queryobs_getEnumerator(state1.obs, true, ERROR_INFO(error));
    REQUIRE(e1);
    CHECK(error.code == 0);
    CHECK(c4queryenum_getRowCount(e1, WITH_ERROR(&error)) == 8);
    state1.count = 0;

    CHECK(state2.count == 1);
    c4::ref<C4QueryEnumerator> e2 = c4queryobs_getEnumerator(state2.obs, true, ERROR_INFO(error));
    REQUIRE(e2);
    CHECK(error.code == 0);
    CHECK(e2 != e1);
    CHECK(c4queryenum_getRowCount(e2, WITH_ERROR(&error)) == 8);
    state2.count = 0;
    
    State state3;
    state3.query = query;
    state3.obs = c4queryobs_create(query, callback, &state3);
    CHECK(state3.obs);
    c4queryobs_setEnabled(state3.obs, true);
    
    C4Log("---- Waiting for a new query observer...");
    REQUIRE_BEFORE(2000ms, state3.count > 0);
    
    C4Log("Checking a new query observer...");
    CHECK(state3.count == 1);
    c4::ref<C4QueryEnumerator> e3 = c4queryobs_getEnumerator(state3.obs, true, ERROR_INFO(error));
    REQUIRE(e3);
    CHECK(error.code == 0);
    CHECK(e3 != e2);
    CHECK(c4queryenum_getRowCount(e3, WITH_ERROR(&error)) == 8);
    state3.count = 0;

    C4Log("Iterating all query results...");
    int count = 0;
    while (c4queryenum_next(e1, nullptr) && c4queryenum_next(e2, nullptr) && c4queryenum_next(e3, nullptr)) {
        ++count;
        FLArrayIterator col1 = e1->columns;
        FLArrayIterator col2 = e2->columns;
        FLArrayIterator col3 = e3->columns;
        auto c = FLArrayIterator_GetCount(&col1);
        CHECK(c == FLArrayIterator_GetCount(&col2));
        CHECK(c == FLArrayIterator_GetCount(&col3));
        for (auto i = 0; i < c; ++i) {
            FLValue v1 = FLArrayIterator_GetValueAt(&col1, i);
            FLValue v2 = FLArrayIterator_GetValueAt(&col2, i);
            FLValue v3 = FLArrayIterator_GetValueAt(&col3, i);
            CHECK(FLValue_IsEqual(v1, v2));
            CHECK(FLValue_IsEqual(v2, v3));
        }
    }
    CHECK(count == 8);
}


#pragma mark - BIGGER DATABASE:


class BigDBQueryTest : public C4QueryTest {
public:
    BigDBQueryTest(int which) :C4QueryTest(which, "iTunesMusicLibrary.json") { }
};


N_WAY_TEST_CASE_METHOD(BigDBQueryTest, "C4Database Optimize", "[Database][C]") {
    C4Log("Creating index...");
    REQUIRE(c4db_createIndex(db, C4STR("byArtist"),
                             R"([[".Artist"], [".Album"], [".Track Number"]])"_sl,
                             kC4ValueIndex, nullptr, WITH_ERROR()));
    C4Log("Incremental optimize...");
    REQUIRE(c4db_maintenance(db, kC4QuickOptimize, WITH_ERROR()));
    C4Log("Full optimize...");
    REQUIRE(c4db_maintenance(db, kC4FullOptimize, WITH_ERROR()));
}


#pragma mark - COLLATION:


class CollatedQueryTest : public BigDBQueryTest {
public:
    CollatedQueryTest(int which) :BigDBQueryTest(which) { }

    vector<string> run() {
        C4Error error;
        c4::ref<C4QueryEnumerator> e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, ERROR_INFO(error));
        REQUIRE(e);
        vector<string> results;
        while (c4queryenum_next(e, ERROR_INFO(error))) {
            string result = Array::iterator(e->columns)[0].asstring();
            results.push_back(result);
        }
        return results;
    }
};


N_WAY_TEST_CASE_METHOD(CollatedQueryTest, "C4Query collated", "[Query][C]") {
    compileSelect(json5("{WHAT: [ ['.Name'] ], \
                         WHERE: ['COLLATE', {'unicode': true, 'case': false, 'diac': false},\
                                            ['=', ['.Artist'], 'Benoît Pioulard']],\
                      ORDER_BY: [ ['COLLATE', {'unicode': true, 'case': false, 'diac': false}, \
                                                ['.Name']] ]}"));

    vector<string> tracks = run();
    CHECK(tracks.size() == 2);
}


N_WAY_TEST_CASE_METHOD(CollatedQueryTest, "C4Query aggregate collated", "[Query][C]") {
    compileSelect(json5("{WHAT: [ ['COLLATE', {'unicode': true, 'case': false, 'diac': false}, \
                                              ['.Artist']] ], \
                      DISTINCT: true, \
                      ORDER_BY: [ ['COLLATE', {'unicode': true, 'case': false, 'diac': false}, \
                                              ['.Artist']] ]}"));

    vector<string> artists = run();
    CHECK(artists.size() == 2094);

    // Benoît Pioulard appears twice in the database, once miscapitalized as BenoÎt Pioulard.
    // Check that these got coalesced by the DISTINCT operator:
    CHECK(artists[214] == "Benny Goodman");
    CHECK(artists[215] == "Benoît Pioulard");
    CHECK(artists[216] == "Bernhard Weiss");

    // Make sure "Zoë Keating" sorts correctly:
    CHECK(artists[2079] == "ZENИTH (feat. saåad)");
    CHECK(artists[2080] == "Zoë Keating");
    CHECK(artists[2081] == "Zola Jesus");
}
