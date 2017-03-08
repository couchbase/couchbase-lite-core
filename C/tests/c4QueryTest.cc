//
//  c4QueryTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 12/16/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4DBQuery.h"
#include <iostream>

using namespace std;


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

    C4Query* compile(const std::string &whereExpr, const std::string &sortExpr ="") {
        string queryStr;
        if (sortExpr.empty()) {
            queryStr = whereExpr;
        } else {
            queryStr = string("[\"SELECT\", {\"WHERE\": ") + whereExpr
                                        + ", \"ORDER_BY\": " + sortExpr + "}]";
        }
        INFO("Query = " << queryStr);
        C4Error error;
        c4query_free(query);
        query = c4query_new(db, c4str(queryStr.c_str()), &error);
        char errbuf[256];
        INFO("error " << error.domain << "/" << error.code << ": " << c4error_getMessageC(error, errbuf, sizeof(errbuf)));
        REQUIRE(query);
        return query;
    }

    std::vector<std::string> run(uint64_t skip =0, uint64_t limit =UINT64_MAX,
                                 const char *bindings =nullptr)
    {
        REQUIRE(query);
        std::vector<std::string> docIDs;
        C4QueryOptions options = kC4DefaultQueryOptions;
        options.skip = skip;
        options.limit = limit;
        C4Error error;
        auto e = c4query_run(query, &options, c4str(bindings), &error);
        INFO("c4query_run got error " << error.domain << "/" << error.code);
        REQUIRE(e);
        while (c4queryenum_next(e, &error)) {
            std::string docID((const char*)e->docID.buf, e->docID.size);
            docIDs.push_back(docID);
        }
        CHECK(error.code == 0);
        c4queryenum_free(e);
        return docIDs;
    }

protected:
    C4Query *query {nullptr};
};


N_WAY_TEST_CASE_METHOD(QueryTest, "Query parser error messages", "[Query][C][!throws]") {
    C4Error error;
    query = c4query_new(db, c4str("[\"=\"]"), &error);
    REQUIRE(query == nullptr);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorInvalidQuery);
    C4StringResult msg = c4error_getMessage(error);
    CHECK(string((char*)msg.buf, msg.size) == "Wrong number of arguments to =");
    c4slice_free(msg);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
    CHECK(run() == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
    CHECK(run(1, 8) == (vector<string>{"0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
    CHECK(run(1, 4) == (vector<string>{"0000015", "0000036", "0000043", "0000053"}));

    compile(json5("['AND', ['=', ['array_count()', ['.', 'contact', 'phone']], 2],\
                           ['=', ['.', 'gender'], 'male']]"));
    CHECK(run() == (vector<string>{"0000002", "0000014", "0000017", "0000027", "0000031", "0000033", "0000038", "0000039", "0000045", "0000047",
        "0000049", "0000056", "0000063", "0000065", "0000075", "0000082", "0000089", "0000094", "0000097"}));

    // MISSING means no value is present (at that array index or dict key)
    compile(json5("['IS', ['.', 'contact', 'phone', [0]], ['MISSING']]"));
    CHECK(run(0, 4) == (vector<string>{"0000004", "0000006", "0000008", "0000015"}));

    // ...wherease null is a JSON null value
    compile(json5("['IS', ['.', 'contact', 'phone', [0]], null]"));
    CHECK(run(0, 4) == (vector<string>{}));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query sorted", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"),
            json5("[['.', 'name', 'last']]"));
    CHECK(run() == (vector<string>{"0000015", "0000036", "0000072", "0000043", "0000001", "0000064", "0000073", "0000053"}));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query bindings", "[Query][C]") {
    compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 1]]"));
    CHECK(run(0, UINT64_MAX, "{\"1\": \"CA\"}") == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
    compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 'state']]"));
    CHECK(run(0, UINT64_MAX, "{\"state\": \"CA\"}") == (vector<string>{"0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"}));
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

    // Look for people where every like contains an L:
    compile(json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['LIKE', ['?', 'like'], '%l%']]"));
    CHECK(run() == (vector<string>{ "0000017", "0000027", "0000060", "0000068" }));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query expression index", "[Query][C]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, &err));
    compile(json5("['=', ['length()', ['.name.first']], 9]"));
    CHECK(run(0, UINT64_MAX) == (vector<string>{ "0000015", "0000099" }));

}


N_WAY_TEST_CASE_METHOD(QueryTest, "Delete indexed doc", "[Query][C]") {
    // Create the same index as the above test:
    C4Error err;
    REQUIRE(c4db_createIndex(db, c4str(json5("[['length()', ['.name.first']]]").c_str()), kC4ValueIndex, nullptr, &err));

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
    CHECK(run(0, UINT64_MAX) == (vector<string>{ "0000099" }));
}


N_WAY_TEST_CASE_METHOD(QueryTest, "Full-text query", "[Query][C]") {
    C4Error err;
    REQUIRE(c4db_createIndex(db, C4STR("[[\".contact.address.street\"]]"), kC4FullTextIndex, nullptr, &err));
    compile(json5("['MATCH', ['.', 'contact', 'address', 'street'], 'Hwy']"));
    CHECK(run(0, UINT64_MAX) == (vector<string>{"0000013", "0000015", "0000043", "0000044", "0000052"}));

}


static string getColumn(C4SliceResult customColumns, unsigned i) {
    REQUIRE(customColumns.buf);
    Array colsArray = Value::fromData((FLSlice)customColumns).asArray();
    REQUIRE(colsArray.count() >= i+1);
    auto s = colsArray[i].asString();
    REQUIRE(s.buf);
    return asstring(s);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query WHAT", "[Query][C]") {
    vector<string> expectedFirst = {"Cleveland", "Georgetta", "Margaretta"};
    vector<string> expectedLast  = {"Bejcek",    "Kolding",   "Ogwynn"};
    compile(json5("{WHAT: ['.name.first', '.name.last'], \
                   WHERE: ['>=', ['length()', ['.name.first']], 9],\
                ORDER_BY: [['.name.first']]}"));
    C4Error error;
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
        auto customColumns = c4queryenum_customColumns(e);
        CHECK(getColumn(customColumns, 0) == expectedFirst[i]);
        CHECK(getColumn(customColumns, 1)  == expectedLast[i]);
        c4slice_free(customColumns);
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == 3);
    c4queryenum_free(e);
}


N_WAY_TEST_CASE_METHOD(QueryTest, "DB Query Aggregate", "[Query][C]") {
    compile(json5("{WHAT: [['min()', ['.name.last']], ['max()', ['.name.last']]]}"));
    C4Error error;
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
        auto customColumns = c4queryenum_customColumns(e);
        CHECK(getColumn(customColumns, 0) == "Aerni");
        CHECK(getColumn(customColumns, 1) == "Zirk");
        c4slice_free(customColumns);
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

    compile(json5("{WHAT: [['.contact.address.state'],\
                           ['min()', ['.name.last']],\
                           ['max()', ['.name.last']]],\
                GROUP_BY: [['.contact.address.state']]}"));
    C4Error error {};
    auto e = c4query_run(query, &kC4DefaultQueryOptions, kC4SliceNull, &error);
    INFO("c4query_run got error " << error.domain << "/" << error.code);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
        auto customColumns = c4queryenum_customColumns(e);
        C4Log("state=%s, first=%s, last=%s", getColumn(customColumns, 0).c_str(), getColumn(customColumns, 1).c_str(), getColumn(customColumns, 2).c_str());
        if (i < expectedState.size()) {
            CHECK(getColumn(customColumns, 0) == expectedState[i]);
            CHECK(getColumn(customColumns, 1) == expectedMin[i]);
            CHECK(getColumn(customColumns, 2) == expectedMax[i]);
        }
        c4slice_free(customColumns);
        ++i;
    }
    CHECK(error.code == 0);
    CHECK(i == expectedRowCount);
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
