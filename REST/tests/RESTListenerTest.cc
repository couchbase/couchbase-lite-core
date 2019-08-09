//
// ListenerTest.cc
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

#include "c4Test.hh"
#include "c4Listener.h"
#include "c4.hh"
#include "FilePath.hh"
#include "Response.hh"
#include "Certificate.hh"

using namespace std;
using namespace fleece;
using namespace litecore::REST;
using namespace litecore::crypto;


#ifdef COUCHBASE_ENTERPRISE


static string to_str(FLSlice s) {
    return string((char*)s.buf, s.size);
}

static string to_str(Value v) {
    return to_str(v.asString());
}


static Retained<Identity> sTemporaryIdentity, sPersistentIdentity;


class C4RESTTest : public C4Test {
public:

    C4RESTTest() :C4Test(0)
    { }


    void setUpDirectory() {
        litecore::FilePath tempDir(TempDir() + "rest/");
        tempDir.delRecursive();
        tempDir.mkdir();
        directory = alloc_slice(tempDir.path().c_str());
        config.directory = directory;
        config.allowCreateDBs = true;
    }


    void useIdentity(Identity *id) {
        configCertData = id->cert->data();
        tlsConfig.certificate = configCertData;
        if (id->privateKey->isPrivateKeyDataAvailable()) {
            configKeyData = id->privateKey->privateKeyData();
            tlsConfig.privateKey = configKeyData;
            tlsConfig.privateKeyRepresentation = kC4PrivateKeyData;
        } else {
            tlsConfig.privateKeyRepresentation = kC4PrivateKeyFromCert;
        }
        config.tlsConfig = &tlsConfig;

    }


    void useTLSWithTemporaryKey() {
        C4Log("Using TLS w/temporary key for this test");
        if (!sTemporaryIdentity) {
            C4Log("Generating TLS key-pair and cert...")
            Retained<PrivateKey> key = PrivateKey::generateTemporaryRSA(2048);
            Cert::IssuerParameters issuerParams;
            issuerParams.validity_secs = 3600*24;
            auto cert = retained(new Cert("CN=C4RESTTest, O=Couchbase, OU=Mobile", issuerParams, key));
            sTemporaryIdentity = new Identity(cert, key);
        }
        useIdentity(sTemporaryIdentity);
    }


    void useTLSWithPersistentKey() {
        C4Log("Using TLS w/persistent key for this test");
        if (!sPersistentIdentity) {
            Retained<PersistentPrivateKey> key = PersistentPrivateKey::generateRSA(2048);
            Cert::IssuerParameters issuerParams;
            issuerParams.validity_secs = 3600*24;
            auto cert = retained(new Cert("CN=C4RESTTest, O=Couchbase, OU=Mobile", issuerParams, key));
            cert->makePersistent();
            sPersistentIdentity = new Identity(cert, key);
        }
        useIdentity(sPersistentIdentity);
    }


    void start() {
        if (listener)
            return;
        if ((c4listener_availableAPIs() & kC4RESTAPI) == 0)
            FAIL("REST API is unavailable in this build");
        C4Error err;
        listener = c4listener_start(&config, &err);
        REQUIRE(listener);

        REQUIRE(c4listener_shareDB(listener, C4STR("db"), db));
    }


    unique_ptr<Response> request(string method, string uri, map<string,string> headersMap, slice body, HTTPStatus expectedStatus) {
        Encoder enc;
        enc.beginDict();
        for (auto &h : headersMap) {
            enc.writeKey(h.first);
            enc.writeString(h.second);
        }
        enc.endDict();
        auto headers = enc.finishDoc();

        start();

        C4Log("---- %s %s", method.c_str(), uri.c_str());
        string scheme = config.tlsConfig ? "https" : "http";
        unique_ptr<Response> r(new Response(scheme, method, "localhost", config.port, uri, headers, body, pinnedCert));
        REQUIRE(r);
        if (r->error().code)
            C4LogToAt(kC4DefaultLog, kC4LogWarning, "Error: %s", c4error_descriptionStr(r->error()));
        C4Log("Status: %d %s", r->status(), r->statusMessage().c_str());
        string responseBody = r->body().asString();
        C4Log("Body: %s", responseBody.c_str());
        INFO("Error: " << c4error_descriptionStr(r->error()));
        REQUIRE(r->status() == expectedStatus);
        return r;
    }

    unique_ptr<Response> request(string method, string uri, HTTPStatus expectedStatus) {
        return request(method, uri, {}, nullslice, expectedStatus);
    }

    C4ListenerConfig config = {59849, kC4RESTAPI};
    alloc_slice directory;
    c4::ref<C4Listener> listener;

    C4TLSConfig tlsConfig = { };
    alloc_slice configCertData, configKeyData;
    Retained<Cert> pinnedCert;
};


#pragma mark - ROOT LEVEL:


TEST_CASE_METHOD(C4RESTTest, "REST root level", "[REST][C]") {
    auto r = request("GET", "/", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    CHECK(to_str(body["couchdb"]) == "Welcome");
}


TEST_CASE_METHOD(C4RESTTest, "REST _all_databases", "[REST][C]") {
    auto r = request("GET", "/_all_dbs", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asArray();
    REQUIRE(body.count() == 1);
    CHECK(to_str(body[0]) == "db");
}


TEST_CASE_METHOD(C4RESTTest, "REST unknown special top-level", "[REST][C]") {
    request("GET", "/_foo", HTTPStatus::NotFound);
    request("GET", "/_", HTTPStatus::NotFound);
}


#pragma mark - DATABASE:


TEST_CASE_METHOD(C4RESTTest, "REST GET database", "[REST][C]") {
    unique_ptr<Response> r;
    SECTION("No slash") {
        r = request("GET", "/db", HTTPStatus::OK);
    }
    SECTION("URL-encoded") {
        r = request("GET", "/%64%62", HTTPStatus::OK);
    }
    SECTION("With slash") {
        r = request("GET", "/db/", HTTPStatus::OK);
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
        r = request("DELETE", "/db", HTTPStatus::Forbidden);
    }
    SECTION("Allowed") {
        config.allowDeleteDBs = true;
        r = request("DELETE", "/db", HTTPStatus::OK);
        r = request("GET", "/db", HTTPStatus::NotFound);
        // This is the easiest cross-platform way to check that the db was deleted:
        REQUIRE(remove(databasePathString().c_str()) != 0);
        REQUIRE(errno == ENOENT);
    }
}


TEST_CASE_METHOD(C4RESTTest, "REST PUT database", "[REST][C]") {
    unique_ptr<Response> r;
    SECTION("Disallowed") {
        r = request("PUT", "/db", HTTPStatus::Forbidden);
        r = request("PUT", "/otherdb", HTTPStatus::Forbidden);
//        r = request("PUT", "/and%2For", HTTPStatus::Forbidden);       // that's a slash. This is a legal db name.
    }
    SECTION("Allowed") {
        setUpDirectory();
        SECTION("Duplicate") {
            r = request("PUT", "/db", HTTPStatus::PreconditionFailed);
        }
        SECTION("New DB") {
            r = request("PUT", "/otherdb", HTTPStatus::Created);
            r = request("GET", "/otherdb", HTTPStatus::OK);

            SECTION("Test _all_dbs again") {
                r = request("GET", "/_all_dbs", HTTPStatus::OK);
                auto body = r->bodyAsJSON().asArray();
                REQUIRE(body.count() == 2);
                CHECK(to_str(body[0]) == "db");
                CHECK(to_str(body[1]) == "otherdb");
            }
        }
    }
}


#pragma mark - DOCUMENTS:


TEST_CASE_METHOD(C4RESTTest, "REST CRUD", "[REST][C]") {
    unique_ptr<Response> r;
    Dict body;
    alloc_slice docID;

    SECTION("POST") {
        r = request("POST", "/db",
                    {{"Content-Type", "application/json"}},
                    "{\"year\": 1964}"_sl, HTTPStatus::Created);
        body = r->bodyAsJSON().asDict();
        docID = body["id"].asString();
        CHECK(docID.size >= 20);
    }

    SECTION("PUT") {
        r = request("PUT", "/db/mydocument",
                    {{"Content-Type", "application/json"}},
                    "{\"year\": 1964}"_sl, HTTPStatus::Created);
        body = r->bodyAsJSON().asDict();
        docID = body["id"].asString();
        CHECK(docID == "mydocument"_sl);

        request("PUT", "/db/mydocument",
                {{"Content-Type", "application/json"}},
                "{\"year\": 1977}"_sl, HTTPStatus::Conflict);
        request("PUT", "/db/mydocument",
                {{"Content-Type", "application/json"}},
                "{\"year\": 1977, \"_rev\":\"1-ffff\"}"_sl, HTTPStatus::Conflict);
    }

    CHECK(body["ok"].asBool() == true);
    alloc_slice revID( body["rev"].asString() );
    CHECK(revID.size > 0);

    {
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(db, docID, true, &err);
        REQUIRE(doc);
        CHECK(doc->revID == revID);
        body = Value::fromData(doc->selectedRev.body).asDict();
        CHECK(body["year"].asInt() == 1964);
        CHECK(body.count() == 1);       // i.e. no _id or _rev properties
    }

    r = request("GET", "/db/" + docID.asString(), HTTPStatus::OK);
    body = r->bodyAsJSON().asDict();
    CHECK(body["_id"].asString() == docID);
    CHECK(body["_rev"].asString() == revID);
    CHECK(body["year"].asInt() == 1964);

    r = request("DELETE", "/db/" + docID.asString() + "?rev=" + revID.asString(), HTTPStatus::OK);
    body = r->bodyAsJSON().asDict();
    CHECK(body["ok"].asBool() == true);
    revID = body["rev"].asString();

    {
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(db, docID, true, &err);
        REQUIRE(doc);
        CHECK((doc->flags & kDocDeleted) != 0);
        CHECK(doc->revID == revID);
        body = Value::fromData(doc->selectedRev.body).asDict();
        CHECK(body.count() == 0);
    }

    r = request("GET", "/db/" + docID.asString(), HTTPStatus::NotFound);
}


TEST_CASE_METHOD(C4RESTTest, "REST _all_docs", "[REST][C]") {
    auto r = request("GET", "/db/_all_docs", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asDict();
    auto rows = body["rows"].asArray();
    CHECK(rows);
    CHECK(rows.count() == 0);

    request("PUT", "/db/mydocument",
            {{"Content-Type", "application/json"}},
            "{\"year\": 1964}"_sl, HTTPStatus::Created);
    request("PUT", "/db/foo",
            {{"Content-Type", "application/json"}},
            "{\"age\": 17}"_sl, HTTPStatus::Created);

    r = request("GET", "/db/_all_docs", HTTPStatus::OK);
    body = r->bodyAsJSON().asDict();
    rows = body["rows"].asArray();
    CHECK(rows);
    CHECK(rows.count() == 2);
    auto row = rows[0].asDict();
    CHECK(row["key"].asString() == "foo"_sl);
    row = rows[1].asDict();
    CHECK(row["key"].asString() == "mydocument"_sl);
}


TEST_CASE_METHOD(C4RESTTest, "REST _bulk_docs", "[REST][C]") {
    unique_ptr<Response> r;
    r = request("POST", "/db/_bulk_docs",
                {{"Content-Type", "application/json"}},
                json5("{docs:[{year:1962}, "
                             "{_id:'jens', year:1964}, "
                             "{_id:'bob', _rev:'1-eeee', year:1900}]}"),
                HTTPStatus::OK);
    Array body = r->bodyAsJSON().asArray();
    CHECK(body.count() == 3);
    
    Dict doc = body[0].asDict();
    CHECK(doc);
    CHECK(doc["ok"].asBool());
    CHECK(doc["id"].asString().size > 0);
    CHECK(doc["rev"].asString().size > 0);

    doc = body[1].asDict();
    CHECK(doc);
    CHECK(doc["ok"].asBool());
    CHECK(doc["id"].asString() == "jens"_sl);
    CHECK(doc["rev"].asString().size > 0);

    doc = body[2].asDict();
    CHECK(doc);
    CHECK(!doc["ok"]);
    CHECK(!doc["id"]);
    CHECK(!doc["rev"]);
    CHECK(doc["status"].asInt() == 404);
    CHECK(doc["error"].asString() == "Not Found"_sl);
}


#pragma mark - TLS:


TEST_CASE_METHOD(C4RESTTest, "TLS REST untrusted cert", "[REST][TLS][C]") {
    useTLSWithTemporaryKey();
    auto r = request("GET", "/", HTTPStatus::undefined);
    CHECK(r->error() == (C4Error{NetworkDomain, kC4NetErrTLSCertUnknownRoot}));
}


TEST_CASE_METHOD(C4RESTTest, "TLS REST pinned cert", "[REST][TLS][C]") {
    useTLSWithTemporaryKey();
    pinnedCert = sTemporaryIdentity->cert;
    auto r = request("GET", "/", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    CHECK(to_str(body["couchdb"]) == "Welcome");
}


TEST_CASE_METHOD(C4RESTTest, "TLS REST pinned cert persistent key", "[REST][TLS][C]") {
    useTLSWithPersistentKey();
    pinnedCert = sPersistentIdentity->cert;
    auto r = request("GET", "/", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    CHECK(to_str(body["couchdb"]) == "Welcome");
}


#endif // COUCHBASE_ENTERPRISE
