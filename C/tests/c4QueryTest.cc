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

#include "c4Test.hh"
#include "c4Query.h"
#include "c4.hh"
#include "c4Document+Fleece.h"
#include <iostream>

using namespace std;
using namespace fleece;


static bool operator==(C4FullTextMatch a, C4FullTextMatch b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static ostream& operator<< (ostream& o, C4FullTextMatch match) {
    return o << "{ds " << match.dataSource << ", prop " << match.property << ", term " << match.term << ", "
             << "bytes " << match.start << " + " << match.length << "}";
}


class QueryTest : public C4Test {
public:
    QueryTest(int which, string filename)
    :C4Test(which)
    {
        importJSONLines(sFixturesDir + filename);
    }

    QueryTest(int which)
    :QueryTest(which, "names_100.json")
    { }

    ~QueryTest() {
        c4query_free(query);
    }

    void compileSelect(const string &queryStr) {
        INFO("Query = " << queryStr);
        C4Error error;
        c4query_free(query);
        query = c4query_new(db, c4str(queryStr.c_str()), &error);
        char errbuf[256];
        INFO("error " << error.domain << "/" << error.code << ": " << c4error_getMessageC(error, errbuf, sizeof(errbuf)));
        REQUIRE(query);
    }

    void compile(const string &whereExpr,
                 const string &sortExpr ="",
                 bool addOffsetLimit =false)
    {
        stringstream json;
        json << "[\"SELECT\", {\"WHAT\": [[\"._id\"]], \"WHERE\": " << whereExpr;
        if (!sortExpr.empty())
            json << ", \"ORDER_BY\": " << sortExpr;
        if (addOffsetLimit)
            json << ", \"OFFSET\": [\"$offset\"], \"LIMIT\":  [\"$limit\"]";
        json << "}]";
        compileSelect(json.str());
    }


    // Runs query, invoking callback for each row and collecting its return values into a vector
    template <class Collected>
    vector<Collected> runCollecting(const char *bindings,
                                    function<Collected(C4QueryEnumerator*)> callback)
    {
        REQUIRE(query);
        C4QueryOptions options = kC4DefaultQueryOptions;
        C4Error error;
        auto e = c4query_run(query, &options, c4str(bindings), &error);
        INFO("c4query_run got error " << error.domain << "/" << error.code);
        REQUIRE(e);
        vector<Collected> results;
        while (c4queryenum_next(e, &error))
            results.push_back(callback(e));
        CHECK(error.code == 0);
        c4queryenum_free(e);
        return results;
    }

    // Runs query, returning vector of doc IDs
    vector<string> run(const char *bindings =nullptr) {
        return runCollecting<string>(bindings, [&](C4QueryEnumerator *e) {
            REQUIRE(FLArrayIterator_GetCount(&e->columns) > 0);
            fleece::slice docID = FLValue_AsString(FLArrayIterator_GetValueAt(&e->columns, 0));
            return docID.asString();
        });
    }

    // Runs query, returning vectors of FTS matches (one vector per row)
    vector<vector<C4FullTextMatch>> runFTS(const char *bindings =nullptr) {
        return runCollecting<vector<C4FullTextMatch>>(bindings, [&](C4QueryEnumerator *e) {
            return vector<C4FullTextMatch>(&e->fullTextMatches[0],
                                           &e->fullTextMatches[e->fullTextMatchCount]);
        });
    }

protected:
    C4Query *query {nullptr};
};


class PathsQueryTest : public QueryTest {
public:
    PathsQueryTest(int which)
    :QueryTest(which, "paths.json")
    { }
};


#pragma mark - TESTS:

N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query", "[Query][C]") {
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
}

N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query LIKE", "[Query][C]") {
    compile(json5("['LIKE', ['.name.first'], '%j%']"));
    CHECK(run() == (vector<string>{ "0000085" }));
    compile(json5("['LIKE', ['.name.first'], '%J%']"));
    CHECK(run() == (vector<string>{ "0000002", "0000004", "0000008", "0000017", "0000028", "0000030", "0000045", "0000052", "0000067", "0000071",
        "0000088", "0000094" }));
    compile(json5("['LIKE', ['.name.first'], 'Jen%']"));
    CHECK(run() == (vector<string>{ "0000008", "0000028" }));
}

N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query IN", "[Query][C]") {
    // Type 1: RHS is an expression; generates a call to array_contains
    compile(json5("['IN', 'reading', ['.', 'likes']]"));
    CHECK(run() == (vector<string>{"0000004", "0000056", "0000064", "0000079", "0000099"}));

    // Type 2: RHS is an array literal; generates a SQL "IN" expression
    compile(json5("['IN', ['.', 'name', 'first'], ['[]', 'Eddie', 'Verna']]"));
    CHECK(run() == (vector<string>{"0000091", "0000093"}));
}

N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query sorted", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"),
            json5("[['.', 'name', 'last']]"));
    CHECK(run() == (vector<string>{"0000015", "0000036", "0000072", "0000043", "0000001", "0000064", "0000073", "0000053"}));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query bindings", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 1]]"));
    CHECK(run("{\"1\": \"CA\"}") == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
    compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 'state']]"));
    CHECK(run("{\"state\": \"CA\"}") == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query ANY", "[Query][C]") {
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


N_WAY_TEST_CASE_METHOD(PathsQueryTest, "DB Query ANY w/paths", "[Query][C]") {
    // For https://github.com/couchbase/couchbase-lite-core/issues/238
    compile(json5("['ANY','path',['.paths'],['=',['?path','city'],'San Jose']]"));
    CHECK(run() == (vector<string>{ "0000001" }));

    compile(json5("['ANY','path',['.paths'],['=',['?path.city'],'San Jose']]"));
    CHECK(run() == (vector<string>{ "0000001" }));

    compile(json5("['ANY','path',['.paths'],['=',['?path','city'],'Palo Alto']]"));
    CHECK(run() == (vector<string>{ "0000001", "0000002" }));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query ANY of dict", "[Query][C]") {
    compile(json5("['ANY', 'n', ['.', 'name'], ['=', ['?', 'n'], 'Arturo']]"));
    CHECK(run() == (vector<string>{"0000090"}));
    compile(json5("['ANY', 'n', ['.', 'name'], ['contains()', ['?', 'n'], 'V']]"));
    CHECK(run() == (vector<string>{ "0000044", "0000048", "0000053", "0000093" }));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query expression index", "[Query][C]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("length"), c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, &err));
    compile(json5("['=', ['length()', ['.name.first']], 9]"));
    CHECK(run() == (vector<string>{ "0000015", "0000099" }));

}


N_WAY_TEST_CASE_METHOD(QueryTest, "Delete indexed doc", "[Query][C]") {
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
        c4doc_free(doc);
        c4doc_free(updatedDoc);
    }

    // Now run a query that would have returned the deleted doc, if it weren't deleted:
    compile(json5("['=', ['length()', ['.name.first']], 9]"));
    CHECK(run() == (vector<string>{ "0000099" }));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Missing columns", "[Query][C]") {
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


#pragma mark - FTS:


N_WAY_TEST_CASE_METHOD(QueryTest, "Full-text query", "[Query][C][FTS]") {
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


N_WAY_TEST_CASE_METHOD(QueryTest, "Full-text multiple properties", "[Query][C][FTS]") {
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


N_WAY_TEST_CASE_METHOD(QueryTest, "Multiple Full-text indexes", "[Query][C][FTS]") {
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


N_WAY_TEST_CASE_METHOD(QueryTest, "Full-text query in multiple ANDs", "[Query][C][FTS]") {
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


N_WAY_TEST_CASE_METHOD(QueryTest, "Multiple Full-text queries", "[Query][C][FTS][!throws]") {
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


N_WAY_TEST_CASE_METHOD(QueryTest, "Buried Full-text queries", "[Query][C][FTS][!throws]") {
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


#pragma mark - WHAT, JOIN, etc:


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query WHAT", "[Query][C]") {
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
    c4queryenum_free(e);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query WHAT returning object", "[Query][C]") {
    auto sk = c4db_getFLSharedKeys(db);
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
        INFO("name = " << name.toJSON(sk));
        CHECK(name.get("first"_sl, sk).asstring() == expectedFirst[i]);
        CHECK(name.get("last"_sl,  sk).asstring() == expectedLast[i]);
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == 3);
    c4queryenum_free(e);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query Aggregate", "[Query][C]") {
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
    c4queryenum_free(e);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query Grouped", "[Query][C]") {
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
    c4queryenum_free(e);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query Join", "[Query][C]") {
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
    c4queryenum_free(e);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query Seek", "[Query][C]") {
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
    c4queryenum_free(e);
}

class NestedQueryTest : public QueryTest {
public:
    NestedQueryTest(int which)
    :QueryTest(which, "nested.json")
    { }
};


N_WAY_TEST_CASE_METHOD(NestedQueryTest, "DB Query ANY nested", "[Query][C]") {
    compile(json5("['ANY', 'Shape', ['.', 'shapes'], ['=', ['?', 'Shape', 'color'], 'red']]"));
    CHECK(run() == (vector<string>{"0000001", "0000003"}));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Query parser error messages", "[Query][C][!throws]") {
    ExpectingExceptions x;

    C4Error error;
    query = c4query_new(db, c4str("[\"=\"]"), &error);
    REQUIRE(query == nullptr);
    CheckError(error, LiteCoreDomain, kC4ErrorInvalidQuery, "Wrong number of arguments to =");
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Query refresh", "[Query][C][!throws]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    C4Error error;
    
    C4SliceResult explanation = c4query_explain(query);
    string explanationString = toString((C4Slice)explanation);
    c4slice_free(explanation);
    CHECK(explanationString.substr(0, 112) == "SELECT fl_result(key) FROM kv_default WHERE (fl_value(body, 'contact.address.state') = 'CA') AND (flags & 1) = 0");
    
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    REQUIRE(e);
    auto refreshed = c4queryenum_refresh(e, &error);
    REQUIRE(!refreshed);
    
    {
        TransactionHelper t(db);
    
        C4Error c4err;
        FLEncoder enc = c4db_getSharedFleeceEncoder(db);
        FLEncoder_BeginDict(enc, 2);
        FLEncoder_WriteKey(enc, FLSTR("custom"));
        FLEncoder_WriteBool(enc, true);
        FLEncoder_WriteKey(enc, FLSTR("contact"));
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("address"));
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("state"));
        FLEncoder_WriteString(enc, FLSTR("CA"));
        FLEncoder_EndDict(enc);
        FLEncoder_EndDict(enc);
        FLEncoder_EndDict(enc);
        
        FLSliceResult body = FLEncoder_Finish(enc, nullptr);
        REQUIRE(body.buf);
        
        // Save document:
        C4DocPutRequest rq = {};
        rq.docID = C4STR("added_later");
        rq.body = (C4Slice)body;
        rq.save = true;
        C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(doc != nullptr);
        c4doc_free(doc);
        FLSliceResult_Free(body);
    }
    
    refreshed = c4queryenum_refresh(e, &error);
    REQUIRE(refreshed);
    c4queryenum_close(e);
    auto count = c4queryenum_getRowCount(refreshed, &error);
    REQUIRE(c4queryenum_seek(refreshed, count - 1, &error));
    CHECK(FLValue_AsString(FLArrayIterator_GetValueAt(&refreshed->columns, 0)) == "added_later"_sl);
    
    c4queryenum_free(e);
    c4queryenum_free(refreshed);
}

N_WAY_TEST_CASE_METHOD(QueryTest, "Delete index", "[Query][C][!throws]") {
    C4Error err;
    C4String names[2] = { C4STR("length"), C4STR("byStreet") };
    string desc1 = json5("[['length()', ['.name.first']]]");
    C4String desc[2] = {c4str(desc1.c_str()), C4STR("[[\".contact.address.street\"]]") };
    C4IndexType types[2] = { kC4ValueIndex, kC4FullTextIndex };
    
    for(int i = 0; i < 2; i++) {
        REQUIRE(c4db_createIndex(db, names[i], desc[i], types[i], nullptr, &err));
        C4SliceResult indexes = c4db_getIndexes(db, &err);
        FLValue val = FLValue_FromTrustedData((FLSlice)indexes);
        REQUIRE(FLValue_GetType(val) == kFLArray);
        FLArray indexArray = FLValue_AsArray(val);
        FLArrayIterator iter;
        FLArrayIterator_Begin(indexArray, &iter);
        REQUIRE(FLArrayIterator_GetCount(&iter) == 1);
        FLString indexName = FLValue_AsString(FLArrayIterator_GetValueAt(&iter, 0));
        CHECK(indexName == names[i]);
        c4slice_free(indexes);
        
        REQUIRE(c4db_deleteIndex(db, names[i], &err));
        indexes = c4db_getIndexes(db, &err);
        val = FLValue_FromTrustedData((FLSlice)indexes);
        REQUIRE(FLValue_GetType(val) == kFLArray);
        indexArray = FLValue_AsArray(val);
        FLArrayIterator_Begin(indexArray, &iter);
        REQUIRE(FLArrayIterator_GetCount(&iter) == 0);
        c4slice_free(indexes);
    }
}

#pragma mark - COLLATION:

class CollatedQueryTest : public QueryTest {
public:
    CollatedQueryTest(int which)
    :QueryTest(which, "iTunesMusicLibrary.json")
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


N_WAY_TEST_CASE_METHOD(CollatedQueryTest, "DB Query collated", "[Query][C]") {
    compileSelect(json5("{WHAT: [ ['.Name'] ], \
                         WHERE: ['COLLATE', {'unicode': true, 'case': false, 'diacritic': false},\
                                            ['=', ['.Artist'], 'Benoît Pioulard']],\
                      ORDER_BY: [ ['COLLATE', {'unicode': true, 'case': false, 'diacritic': false}, \
                                                ['.Name']] ]}"));

    vector<string> tracks = run();
    CHECK(tracks.size() == 2);
}


N_WAY_TEST_CASE_METHOD(CollatedQueryTest, "DB Query aggregate collated", "[Query][C]") {
    compileSelect(json5("{WHAT: [ ['COLLATE', {'unicode': true, 'case': false, 'diacritic': false}, \
                                              ['.Artist']] ], \
                      DISTINCT: true, \
                      ORDER_BY: [ ['COLLATE', {'unicode': true, 'case': false, 'diacritic': false}, \
                                              ['.Artist']] ]}"));

    vector<string> artists = run();
    CHECK(artists.size() == 2097);

    // Benoît Pioulard appears twice in the database, once miscapitalized as BenoÎt Pioulard.
    // Check that these got coalesced by the DISTINCT operator:
    CHECK(artists[214] == "Benny Goodman");
    CHECK(artists[215] == "Benoît Pioulard");
    CHECK(artists[216] == "Bernhard Weiss");

    // Make sure "Zoë Keating" sorts correctly:
    CHECK(artists[2082] == "ZENИTH (feat. saåad)");
    CHECK(artists[2083] == "Zoë Keating");
    CHECK(artists[2084] == "Zola Jesus");
}
