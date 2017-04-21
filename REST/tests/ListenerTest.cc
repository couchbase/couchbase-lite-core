//
//  ListenerTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "C4Test.hh"
#include "c4REST.h"
#include "c4.hh"
#include "Response.hh"

using namespace std;
using namespace litecore::REST;


static string to_str(FLSlice s) {
    return string((char*)s.buf, s.size);
}

static string to_str(Value v) {
    return to_str(v.asString());
}


class C4RESTTest : public C4Test {
public:

    C4RESTTest() :C4Test(0)
    { }


    void start() {
        if (listener)
            return;
        C4Error err;
        listener = c4rest_start(&config, &err);
        REQUIRE(listener);

        c4rest_shareDB(listener, C4STR("db"), db);
    }


    unique_ptr<Response> request(string method, string uri, int expectedStatus) {
        start();
        C4Log("---- %s %s", method.c_str(), uri.c_str());
        unique_ptr<Response> r(new Response(method, "localhost", config.port, uri.c_str()));
        if (!*r)
            INFO("Error is " << r->statusMessage());
        REQUIRE(r);
        C4Log("Status: %d %s", r->status(), r->statusMessage().c_str());
        string body = r->body().asString();
        C4Log("Body: %s", body.c_str());
        REQUIRE(r->status() == expectedStatus);
        return r;
    }


    C4RESTConfig config = {59849 };
    c4::ref<C4RESTListener> listener;
};


#pragma mark - ROOT LEVEL:


TEST_CASE_METHOD(C4RESTTest, "REST root level", "[REST][C]") {
    auto r = request("GET", "/", 200);
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    CHECK(to_str(body["couchdb"]) == "Welcome");
}


TEST_CASE_METHOD(C4RESTTest, "REST _all_databases", "[REST][C]") {
    auto r = request("GET", "/_all_dbs", 200);
    auto body = r->bodyAsJSON().asArray();
    REQUIRE(body.count() == 1);
    CHECK(to_str(body[0]) == "db");
}


TEST_CASE_METHOD(C4RESTTest, "REST unknown special top-level", "[REST][C]") {
    request("GET", "/_foo", 404);
    request("GET", "/_", 404);
}


#pragma mark - DATABASE:


TEST_CASE_METHOD(C4RESTTest, "REST GET database", "[REST][C]") {
    unique_ptr<Response> r;
    SECTION("No slash") {
        r = request("GET", "/db", 200);
    }
    SECTION("URL-encoded") {
        r = request("GET", "/%64%62", 200);
    }
    SECTION("With slash") {
        r = request("GET", "/db/", 200);
    }
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    CHECK(to_str(body["db_name"]) == "db");
    CHECK(body["db_uuid"].type() == kFLString);
    CHECK(body["db_uuid"].asString().size >= 32);
    CHECK(body["doc_count"].type() == kFLNumber);
    CHECK(body["doc_count"].asInt() == 0);
    CHECK(body["update_seq"].type() == kFLNumber);
    CHECK(body["update_seq"].asInt() == 0);
}


TEST_CASE_METHOD(C4RESTTest, "REST DELETE database", "[REST][C]") {
    unique_ptr<Response> r;
    SECTION("Disallowed") {
        r = request("DELETE", "/db", 403);
    }
    SECTION("Allowed") {
        config.allowDeleteDBs = true;
        r = request("DELETE", "/db", 200);
        r = request("GET", "/db", 404);
        // This is the easiest cross-platform way to check that the db was deleted:
        REQUIRE(remove(databasePathString().c_str()) != 0);
        REQUIRE(errno == ENOENT);
    }
}


TEST_CASE_METHOD(C4RESTTest, "REST PUT database", "[REST][C]") {
    unique_ptr<Response> r;
    SECTION("Disallowed") {
        r = request("PUT", "/db", 403);
        r = request("PUT", "/otherdb", 403);
        r = request("PUT", "/and%2For", 403);       // that's a slash. This is a legal db name.
    }
    SECTION("Allowed") {
        config.directory = c4str(TempDir().c_str());
        config.allowCreateDBs = true;
        SECTION("Invalid name") {
            r = request("PUT", "/xDB", 400);
            r = request("PUT", "/uh*oh", 400);
            r = request("PUT", "/23skidoo", 400);
        }
        SECTION("Duplicate") {
            r = request("PUT", "/db", 412);
        }
        SECTION("New DB") {
            r = request("PUT", "/otherdb", 201);
            r = request("GET", "/otherdb", 200);

            SECTION("Test _all_dbs again") {
                r = request("GET", "/_all_dbs", 200);
                auto body = r->bodyAsJSON().asArray();
                REQUIRE(body.count() == 2);
                CHECK(to_str(body[0]) == "db");
                CHECK(to_str(body[1]) == "otherdb");
            }
        }
    }
}


#pragma mark - DOCUMENTS:
