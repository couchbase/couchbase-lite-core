//
// c4QueryTest.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "c4QueryTest.hh"
#include "c4BlobStore.h"
#include "c4Observer.h"
#include "StringUtil.hh"
#include <thread>


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


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query expression index", "[Query][C]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("length"), c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, &err));
    compile(json5("['=', ['length()', ['.name.first']], 9]"));
    CHECK(run() == (vector<string>{ "0000015", "0000099" }));

}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "Delete indexed doc", "[Query][C]") {
    // Create the same index as the above test:
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("length"), c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, &err));

    // Delete doc "0000015":
    {
        TransactionHelper t(db);

        C4Error c4err;
        C4Document *doc = c4doc_get(db, C4STR("0000015"), true, &c4err);
        REQUIRE(doc);
        C4DocPutRequest rq = {};
        rq.docID = C4STR("0000015");
        rq.history = &doc->revID;
        rq.historyCount = 1;
        rq.revFlags = kRevDeleted;
        rq.save = true;
        C4Document *updatedDoc = c4doc_put(db, &rq, nullptr, &c4err);
        INFO("c4err = " << c4err.domain << "/" << c4err.code);
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
    checkColumnTitles({"*", "name", "gender", "zip"});
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
        return string(slice(FLValue_ToJSON5(result)));
    });
    CHECK(results[0] == "{d:1234.5,f:false,i:12345,id:\"0000001\",n:null,s:\"howdy\",t:true}");
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query N1QL parse error", "[Query][C][N1QL][!throws]") {
    int errPos;
    C4Error error;
    {
        ExpectingExceptions x;
        CHECK(c4query_new2(db, kC4N1QLQuery, "SELECT foo bar"_sl, &errPos, &error) == nullptr);
    }
    CHECK(errPos == 11);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorInvalidQuery);

    C4Query *q = c4query_new2(db, kC4N1QLQuery, "SELECT foo, bar"_sl, &errPos, &error);
    CHECK(q != nullptr);
    c4query_release(q);
}

#pragma mark - FTS:


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    compile(json5("['MATCH', 'byStreet', 'Hwy']"));
    auto results = runFTS();
    CHECK(results == (vector<vector<C4FullTextMatch>>{
        {{13, 0, 0, 10, 3}},
        {{15, 0, 0, 11, 3}},
        {{43, 0, 0, 12, 3}},
        {{44, 0, 0, 12, 3}},
        {{52, 0, 0, 11, 3}}
    }));
    
    C4SliceResult matched = c4query_fullTextMatched(query, &results[0][0], &err);
    REQUIRE(matched.buf != nullptr);
    CHECK(toString((C4Slice)matched) == "7 Wyoming Hwy");
    c4slice_free(matched);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS multiple properties", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byAddress"),
                             C4STR("[[\".contact.address.street\"], [\".contact.address.city\"], [\".contact.address.state\"]]"), kC4FullTextIndex, nullptr, &err));
    // Some docs match 'Santa' in the street name, some in the city name
    compile(json5("['MATCH', 'byAddress', 'Santa']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {15, 1, 0, 0, 5} },
        { {44, 0, 0, 3, 5} },
        { {68, 0, 0, 3, 5} },
        { {72, 1, 0, 0, 5} },
    }));

    // Search only the street name:
    compile(json5("['MATCH', 'byAddress', 'contact.address.street:Santa']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {44, 0, 0, 3, 5} },
        { {68, 0, 0, 3, 5} }
    }));

    // Search for 'Santa' in the street name, and 'Saint' in either:
    compile(json5("['MATCH', 'byAddress', 'contact.address.street:Santa Saint']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {68, 0, 0, 3, 5}, {68, 1, 1, 0, 5} }
    }));

    // Search for 'Santa' in the street name, _or_ 'Saint' in either:
    compile(json5("['MATCH', 'byAddress', 'contact.address.street:Santa OR Saint']"));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {20, 1, 1, 0, 5} },
        { {44, 0, 0, 3, 5} },
        { {68, 0, 0, 3, 5}, {68, 1, 1, 0, 5} },
        { {77, 1, 1, 0, 5} }
    }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS Multiple indexes", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    REQUIRE(c4db_createIndex(db, C4STR("byCity"), C4STR("[[\".contact.address.city\"]]"), kC4FullTextIndex, nullptr, &err));
    compile(json5("['AND', ['MATCH', 'byStreet', 'Hwy'],\
                           ['MATCH', 'byCity',   'Santa']]"));
    CHECK(run() == (vector<string>{"0000015"}));
    CHECK(runFTS() == (vector<vector<C4FullTextMatch>>{
        { {15, 0, 0, 11, 3} }
    }));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS multiple ANDs", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    REQUIRE(c4db_createIndex(db, C4STR("byCity"), C4STR("[[\".contact.address.city\"]]"), kC4FullTextIndex, nullptr, &err));
    compile(json5("['AND', ['AND', ['=', ['.gender'], 'male'],\
                                   ['MATCH', 'byCity', 'Santa']],\
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
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    query = c4query_new(db,
                        json5slice("['AND', ['MATCH', 'byStreet', 'Hwy'],\
                                            ['MATCH', 'byStreet', 'Blvd']]"),
                        &err);
    REQUIRE(query == nullptr);
    CheckError(err, LiteCoreDomain, kC4ErrorInvalidQuery,
               "Sorry, multiple MATCHes of the same property are not allowed");
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS Buried", "[Query][C][FTS][!throws]") {
    // You can't put an FTS match inside an expression other than a top-level AND (says SQLite)
    ExpectingExceptions x;
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    query = c4query_new(db,
                        json5slice("['OR', ['MATCH', 'byStreet', 'Hwy'],\
                                           ['=', ['.', 'contact', 'address', 'state'], 'CA']]"),
                        &err);
    REQUIRE(query == nullptr);
    CheckError(err, LiteCoreDomain, kC4ErrorInvalidQuery,
               "MATCH can only appear at top-level, or in a top-level AND");
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS Aggregate", "[Query][C][FTS]") {
    // https://github.com/couchbase/couchbase-lite-core/issues/703
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    query = c4query_new(db,
            json5slice("['SELECT', { 'WHAT': [ [ 'count()', [ '.', 'uuid' ] ] ],"
                       " 'WHERE': [ 'AND', [ 'AND', [ '=', [ '.', 'doc_type' ], 'rec' ],"
                                                  " [ 'MATCH', 'byStreet', 'keyword' ] ],"
                                         "[ '=', [ '.', 'pId' ], 'bfe2970b-9be6-46f6-b9a7-38c5947c27b1' ] ] } ]"),
                        &err);
    // Just test whether the enumerator starts without an error:
    auto e = c4query_run(query, nullptr, nullslice, &err);
    REQUIRE(e);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS with alias", "[Query][C][FTS]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("byStreet"), C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    query = c4query_new(db,
            json5slice("['SELECT', { 'WHAT': [ [ '.db.uuid' ] ],"
                       " 'FROM': [{ 'AS' : 'db'}],"
                       " 'WHERE': [ 'AND', [ 'AND', [ '=', [ '.db.doc_type' ], 'rec' ],"
                                                  " [ 'MATCH', 'byStreet', 'keyword' ] ],"
                                         "[ '=', [ '.db.pId' ], 'bfe2970b-9be6-46f6-b9a7-38c5947c27b1' ] ] } ]"),
                        &err);
    // Just test whether the enumerator starts without an error:
    auto e = c4query_run(query, nullptr, nullslice, &err);
    REQUIRE(e);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query FTS with accents", "[Query][C][FTS]") {
    // https://github.com/couchbase/couchbase-lite-core/issues/723
    C4Error err;
    C4IndexOptions options = {
       nullptr, false, false, nullptr
    };

    REQUIRE(c4db_createIndex(db, C4STR("nameFTSIndex"), C4STR("[[\".content\"]]"), kC4FullTextIndex, &options, &err));

    {
        TransactionHelper t(db);

        C4SliceResult bodyContent = c4db_encodeJSON(db, C4STR(u8"{\"content\": \"Hâkimler\"}"), &err);
        REQUIRE(bodyContent.buf != nullptr);
        createNewRev(db, C4STR("1"), (C4Slice)bodyContent);
        c4slice_free(bodyContent);

        bodyContent = c4db_encodeJSON(db, C4STR("{\"content\": \"Hakimler\"}"), &err);
        REQUIRE(bodyContent.buf != nullptr);
        createNewRev(db, C4STR("2"), (C4Slice)bodyContent);
        c4slice_free(bodyContent);

        bodyContent = c4db_encodeJSON(db, C4STR("{\"content\": \"foo\"}"), &err);
        REQUIRE(bodyContent.buf != nullptr);
        createNewRev(db, C4STR("3"), (C4Slice)bodyContent);
        c4slice_free(bodyContent);
    }

    C4Slice queryStr = C4STR("{\"WHERE\": [\"MATCH\",\"nameFTSIndex\",\"'hâkimler'\"], \"WHAT\": [[\".\"]]}");
    query = c4query_new(db, queryStr, &err);
    auto e = c4query_run(query, nullptr, nullslice, &err);
    REQUIRE(e);
    CHECK(c4queryenum_getRowCount(e, &err) == 1);
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
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
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
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    if (!e)
        INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
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
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
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
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
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
    CHECK(c4queryenum_getRowCount(e, &error) == 42);
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
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
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
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    REQUIRE(e);
    REQUIRE(c4queryenum_next(e, &error));
    REQUIRE(FLArrayIterator_GetCount(&e->columns) > 0);
    FLString docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
    REQUIRE(docID == "0000001"_sl);
    REQUIRE(c4queryenum_next(e, &error));
    REQUIRE(c4queryenum_seek(e, 0, &error));
    docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
    REQUIRE(docID == "0000001"_sl);
    REQUIRE(c4queryenum_seek(e, 7, &error));
    docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
    REQUIRE(docID == "0000073"_sl);
    {
        ExpectingExceptions ex;
        REQUIRE(!c4queryenum_seek(e, 100, &error));
    }
    
    CHECK(error.code == kC4ErrorInvalidParameter);
    CHECK(error.domain == LiteCoreDomain);
    c4queryenum_release(e);
}


N_WAY_TEST_CASE_METHOD(NestedQueryTest, "C4Query ANY nested", "[Query][C]") {
    compile(json5("['ANY', 'Shape', ['.', 'shapes'], ['=', ['?', 'Shape', 'color'], 'red']]"));
    CHECK(run() == (vector<string>{"0000001", "0000003"}));
}


N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query parser error messages", "[Query][C][!throws]") {
    ExpectingExceptions x;

    C4Error error;
    query = c4query_new(db, c4str("[\"=\"]"), &error);
    REQUIRE(query == nullptr);
    CheckError(error, LiteCoreDomain, kC4ErrorInvalidQuery, "Wrong number of arguments to =");
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query refresh", "[Query][C][!throws]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    C4Error error;
    
    C4SliceResult explanation = c4query_explain(query);
    string explanationString = toString((C4Slice)explanation);
    c4slice_free(explanation);
    CHECK(litecore::hasPrefix(explanationString, "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'contact.address.state') = 'CA') AND (_doc.flags & 1 = 0)"));
    
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    REQUIRE(e);
    auto refreshed = c4queryenum_refresh(e, &error);
    REQUIRE(!refreshed);
    
    addPersonInState("added_later", "CA");
    
    refreshed = c4queryenum_refresh(e, &error);
    REQUIRE(refreshed);
    auto count = c4queryenum_getRowCount(refreshed, &error);
    REQUIRE(c4queryenum_seek(refreshed, count - 1, &error));
    CHECK(FLValue_AsString(FLArrayIterator_GetValueAt(&refreshed->columns, 0)) == "added_later"_sl);
    c4queryenum_release(refreshed);

    {
        TransactionHelper t(db);
        REQUIRE(c4db_purgeDoc(db, "added_later"_sl, &error));
    }

    refreshed = c4queryenum_refresh(e, &error);
    REQUIRE(refreshed);
    c4queryenum_close(e);
    count = c4queryenum_getRowCount(refreshed, &error);
    REQUIRE(c4queryenum_seek(refreshed, count - 1, &error));
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
        int count = 0;
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
    WaitUntil(2000, [&]{return state.count > 0;});

    C4Log("Checking query observer...");
    CHECK(state.count == 1);
    c4::ref<C4QueryEnumerator> e = c4queryobs_getEnumerator(state.obs, true, &error);
    REQUIRE(e);
    CHECK(c4queryobs_getEnumerator(state.obs, true, &error) == nullptr);
    CHECK(error.code == 0);
    CHECK(c4queryenum_getRowCount(e, &error) == 8);
    state.count = 0;

    addPersonInState("after1", "AL");

    C4Log("---- Checking that query observer doesn't fire...");
    this_thread::sleep_for(chrono::milliseconds(1000));
    REQUIRE(state.count == 0);

    {
        C4Log("---- Changing a doc in the query");
        TransactionHelper t(db);
        addPersonInState("after2", "CA");
        // wait, to make sure the observer doesn't try to run the query before the commit
        this_thread::sleep_for(chrono::milliseconds(1000));
        C4Log("---- Commiting changes");
    }

    C4Log("---- Waiting for 2nd call of query observer...");
    WaitUntil(2000, [&]{return state.count > 0;});

    C4Log("---- Checking query observer again...");
    CHECK(state.count == 1);
    c4::ref<C4QueryEnumerator> e2 = c4queryobs_getEnumerator(state.obs, false, &error);
    REQUIRE(e2);
    CHECK(e2 != e);
    c4::ref<C4QueryEnumerator> e3 = c4queryobs_getEnumerator(state.obs, false, &error);
    CHECK(e3 == e2);
    CHECK(c4queryenum_getRowCount(e2, &error) == 9);

    // Testing with purged document:
    C4Log("---- Purging a document...");
    state.count = 0;
    {
        TransactionHelper t(db);
        REQUIRE(c4db_purgeDoc(db, "after2"_sl, &error));
        C4Log("---- Commiting changes");
    }

    C4Log("---- Waiting for 3rd call of query observer...");
    WaitUntil(2000, [&]{return state.count > 0;});

    C4Log("---- Checking query observer again...");
    CHECK(state.count == 1);
    e2 = c4queryobs_getEnumerator(state.obs, true, &error);
    REQUIRE(e2);
    CHECK(e2 != e);
    CHECK(c4queryenum_getRowCount(e2, &error) == 8);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "Delete index", "[Query][C][!throws]") {
    C4Error err;
    C4String names[2] = { C4STR("length"), C4STR("byStreet") };
    string desc1 = json5("[['length()', ['.name.first']]]");
    C4String desc[2] = {c4str(desc1.c_str()), C4STR("[[\".contact.address.street\"]]") };
    C4IndexType types[2] = { kC4ValueIndex, kC4FullTextIndex };
    
    for(int i = 0; i < 2; i++) {
        REQUIRE(c4db_createIndex(db, names[i], desc[i], types[i], nullptr, &err));
        C4SliceResult indexes = c4db_getIndexes(db, &err);
        FLValue val = FLValue_FromData((FLSlice)indexes, kFLTrusted);
        REQUIRE(FLValue_GetType(val) == kFLArray);
        FLArray indexArray = FLValue_AsArray(val);
        FLArrayIterator iter;
        FLArrayIterator_Begin(indexArray, &iter);
        REQUIRE(FLArrayIterator_GetCount(&iter) == 1);
        FLString indexName = FLValue_AsString(FLArrayIterator_GetValueAt(&iter, 0));
        CHECK(indexName == names[i]);
        c4slice_free(indexes);
        
        REQUIRE(c4db_deleteIndex(db, names[i], &err));
        indexes = c4db_getIndexesInfo(db, &err);
        val = FLValue_FromData((FLSlice)indexes, kFLTrusted);
        REQUIRE(FLValue_GetType(val) == kFLArray);
        indexArray = FLValue_AsArray(val);
        FLArrayIterator_Begin(indexArray, &iter);
        REQUIRE(FLArrayIterator_GetCount(&iter) == 0);
        c4slice_free(indexes);
    }
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "Database alias column names", "[Query][C][!throws]") {
    // https://github.com/couchbase/couchbase-lite-core/issues/750

    C4Error err;
    string queryText = "{'WHAT':[['.main.'],['.secondary.']],'FROM':[{'AS':'main'},{'AS':'secondary','ON':['=',['.main.number1'],['.secondary.theone']]}]}";
    FLSliceResult queryStr = FLJSON5_ToJSON({queryText.data(), queryText.size()}, nullptr, nullptr, nullptr);
    query = c4query_new(db, (C4Slice)queryStr, &err);
    FLSlice expected1 = FLSTR("main");
    FLSlice expected2 = FLSTR("secondary");
    CHECK(c4query_columnTitle(query, 0) == expected1);
    CHECK(c4query_columnTitle(query, 1) == expected2);
    FLSliceResult_Release(queryStr);
}

N_WAY_TEST_CASE_METHOD(C4QueryTest, "C4Query RevisionID", "[Query][C][!throws]") {
    C4Error error;
    TransactionHelper t(db);
    
    // New Doc:
    auto doc1a = c4doc_create(db, C4STR("doc1"), kC4SliceNull, 0, &error);
    auto revID = toString(doc1a->revID);
    compileSelect(json5("{WHAT: [['._revisionID']], WHERE: ['=', ['._id'], 'doc1']}"));
    CHECK(run() == (vector<string>{revID}));
    
    // revisionID in WHERE:
    compileSelect(json5("{WHAT: [['._id']], WHERE: ['=', ['._revisionID'], '" + revID + "']}"));
    CHECK(run() == (vector<string>{"doc1"}));
    
    // Updated Doc:
    auto doc1b = c4doc_update(doc1a, json2fleece("{'ok':'go'}"), 0, &error);
    revID = toString(doc1b->revID);
    c4doc_release(doc1a);
    compileSelect(json5("{WHAT: [['._revisionID']], WHERE: ['=', ['._id'], 'doc1']}"));
    CHECK(run() == (vector<string>{revID}));
    
    // Deleted Doc:
    auto doc1c = c4doc_update(doc1b, kC4SliceNull, kRevDeleted, &error);
    revID = toString(doc1c->revID);
    c4doc_release(doc1b);
    compileSelect(json5("{WHAT: [['._revisionID']], WHERE: ['AND', ['._deleted'], ['=', ['._id'], 'doc1']]}"));
    CHECK(run() == (vector<string>{revID}));
    c4doc_release(doc1c);
}

#pragma mark - COLLATION:

class CollatedQueryTest : public C4QueryTest {
public:
    CollatedQueryTest(int which)
    :C4QueryTest(which, "iTunesMusicLibrary.json")
    { }

    vector<string> run() {
        C4Error error;
        c4::ref<C4QueryEnumerator> e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
        if (!e)
            INFO("c4query_run got error " << error.domain << "/" << error.code);
        REQUIRE(e);
        vector<string> results;
        while (c4queryenum_next(e, &error)) {
            string result = Array::iterator(e->columns)[0].asstring();
            results.push_back(result);
        }
        CHECK(error.code == 0);
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
