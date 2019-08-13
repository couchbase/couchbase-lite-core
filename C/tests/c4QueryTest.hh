//
//  c4QueryTest.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/3/18.
//  Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once

#include "c4Test.hh"
#include "c4Query.h"
#include "c4.hh"
#include "c4Document+Fleece.h"
#include "StringUtil.hh"
#include <iostream>

using namespace std;
using namespace fleece;


class QueryTest : public C4Test {
public:
    QueryTest(int which, string filename)
    :C4Test(which)
    {
        if (!filename.empty())
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
        INFO("error " << c4error_getDescriptionC(error, errbuf, sizeof(errbuf)));
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

    void checkExplanation(bool indexed =false) {
        alloc_slice explanation = c4query_explain(query);
        C4Log("Explanation: %.*s", SPLAT(explanation));
        if (indexed)
            CHECK(explanation.find("SCAN"_sl) == nullslice);    // should be no linear table scans
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

    // Runs query, returning vector of doc IDs (or whatever 1st result col is)
    vector<string> run(const char *bindings =nullptr) {
        return runCollecting<string>(bindings, [&](C4QueryEnumerator *e) {
            REQUIRE(FLArrayIterator_GetCount(&e->columns) > 0);
            if (e->missingColumns & 1)
                return string("MISSING");
            FLValue val = FLArrayIterator_GetValueAt(&e->columns, 0);
            fleece::alloc_slice result;
            if (FLValue_GetType(val) == kFLString)
                result = FLValue_ToString(val);
            else
                result = FLValue_ToJSON(val);
            return result.asString();
        });
    }

    // Runs query, returning vector of doc IDs
    vector<string> run2(const char *bindings =nullptr) {
        return runCollecting<string>(bindings, [&](C4QueryEnumerator *e) {
            REQUIRE(FLArrayIterator_GetCount(&e->columns) >= 2);
            fleece::alloc_slice c1 = FLValue_ToString(FLArrayIterator_GetValueAt(&e->columns, 0));
            fleece::alloc_slice c2 = FLValue_ToString(FLArrayIterator_GetValueAt(&e->columns, 1));
            if (e->missingColumns & 1)
                c1 = "MISSING"_sl;
            if (e->missingColumns & 2)
                c2 = "MISSING"_sl;
            return c1.asString() + ", " + c2.asString();
        });
    }

    // Runs query, returning vectors of FTS matches (one vector per row)
    vector<vector<C4FullTextMatch>> runFTS(const char *bindings =nullptr) {
        return runCollecting<vector<C4FullTextMatch>>(bindings, [&](C4QueryEnumerator *e) {
            return vector<C4FullTextMatch>(&e->fullTextMatches[0],
                                           &e->fullTextMatches[e->fullTextMatchCount]);
        });
    }

    void checkColumnTitles(const vector<string> &expectedTitles) {
        size_t n = c4query_columnCount(query);
        vector<string> titles;
        for (unsigned i = 0; i < n; ++i)
            titles.push_back( string(slice(c4query_columnTitle(query, i))) );
        CHECK(titles == expectedTitles);
    }

    void addPersonInState(const char *docID, const char *state, const char* firstName = nullptr) {
        TransactionHelper t(db);

        C4Error c4err;
        size_t count = 2 + firstName != nullptr;
        FLEncoder enc = c4db_getSharedFleeceEncoder(db);
        FLEncoder_BeginDict(enc, count);
        FLEncoder_WriteKey(enc, FLSTR("custom"));
        FLEncoder_WriteBool(enc, true);
        if(firstName != nullptr) {
            FLEncoder_WriteKey(enc, FLSTR("name"));
            FLEncoder_BeginDict(enc, 2);
            FLEncoder_WriteKey(enc, FLSTR("first"));
            FLEncoder_WriteString(enc, FLStr(firstName));
            FLEncoder_WriteKey(enc, FLSTR("last"));
            FLEncoder_WriteString(enc, FLSTR("lastname"));
            FLEncoder_EndDict(enc);
        }

        FLEncoder_WriteKey(enc, FLSTR("contact"));
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("address"));
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("state"));
        FLEncoder_WriteString(enc, FLStr(state));
        FLEncoder_EndDict(enc);
        FLEncoder_EndDict(enc);
        FLEncoder_EndDict(enc);

        FLSliceResult body = FLEncoder_Finish(enc, nullptr);
        REQUIRE(body.buf);

        // Save document:
        C4DocPutRequest rq = {};
        rq.docID = slice(docID);
        rq.allocedBody = body;
        rq.save = true;
        C4Document *doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(doc != nullptr);
        c4doc_free(doc);
        FLSliceResult_Free(body);
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

class NestedQueryTest : public QueryTest {
public:
    NestedQueryTest(int which)
    :QueryTest(which, "nested.json")
    { }
};
