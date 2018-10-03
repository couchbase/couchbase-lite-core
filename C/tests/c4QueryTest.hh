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

    // Runs query, returning vector of doc IDs
    vector<string> run(const char *bindings =nullptr) {
        return runCollecting<string>(bindings, [&](C4QueryEnumerator *e) {
            REQUIRE(FLArrayIterator_GetCount(&e->columns) > 0);
            fleece::alloc_slice docID = FLValue_ToString(FLArrayIterator_GetValueAt(&e->columns, 0));
            return docID.asString();
        });
    }

    // Runs query, returning vector of doc IDs
    vector<string> run2(const char *bindings =nullptr) {
        return runCollecting<string>(bindings, [&](C4QueryEnumerator *e) {
            REQUIRE(FLArrayIterator_GetCount(&e->columns) >= 2);
            fleece::alloc_slice c1 = FLValue_ToString(FLArrayIterator_GetValueAt(&e->columns, 0));
            fleece::alloc_slice c2 = FLValue_ToString(FLArrayIterator_GetValueAt(&e->columns, 1));
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
