//
// RESTListenerTest.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"
#include "Error.hh"
#include "c4Collection.h"
#include "c4Database.h"
#include "c4Replicator.h"
#include "ListenerHarness.hh"
#include "FilePath.hh"
#include "Response.hh"
#include "NetworkInterfaces.hh"
#include "fleece/Mutable.hh"
#include "ReplicatorAPITest.hh"
#include <optional>

using namespace litecore;
using namespace litecore::net;
using namespace litecore::REST;
using namespace std;


#if !defined(_WIN32) || defined(_WIN64)
// These tests often hang in 32-bit Windows but let's cheekily ignore it since
// this not part of Couchbase Lite directly anyway, but used in the cblite CLI
// which is 64-bit only on Windows.

static string to_str(FLSlice s) { return string((char*)s.buf, s.size); }

static string to_str(Value v) { return to_str(v.asString()); }

#    define TEST_PORT 0

class C4RESTTest
    : public C4Test
    , public ListenerHarness {
  public:
    C4RESTTest() : C4Test(0), ListenerHarness({TEST_PORT, nullslice, kC4RESTAPI}) {
        std::call_once(ReplicatorAPITest::once, [&]() {
            // Register the BuiltInWebSocket class as the C4Replicator's WebSocketImpl.
            C4RegisterBuiltInWebSocket();
        });
    }

    void setUpDirectory() {
        litecore::FilePath tempDir(TempDir() + "rest/");
        tempDir.delRecursive();
        tempDir.mkdir();
        directory             = alloc_slice(tempDir.path().c_str());
        config.directory      = directory;
        config.allowCreateDBs = true;
    }


#    ifdef COUCHBASE_ENTERPRISE
    void setupCertAuth() {
        auto callback = [](C4Listener* listener, C4Slice clientCertData, void* context) -> bool {
            auto self              = (C4RESTTest*)context;
            self->receivedCertAuth = clientCertData;
            return self->allowClientCert;
        };
        setCertAuthCallback(callback, this);
    }
#    endif


    void setupHTTPAuth() {
        config.callbackContext  = this;
        config.httpAuthCallback = [](C4Listener* listener, C4Slice authHeader, void* context) {
            auto self                          = (C4RESTTest*)context;
            self->receivedHTTPAuthFromListener = listener;
            self->receivedHTTPAuthHeader       = authHeader;
            return self->allowHTTPConnection;
        };
    }

    void forEachURL(C4Database* db, C4ListenerAPIs api, function_ref<void(string_view)> callback) {
        MutableArray urls(c4listener_getURLs(listener(), db, api, nullptr));
        FLMutableArray_Release(urls);
        REQUIRE(urls);
        for ( Array::iterator i(urls); i; ++i ) callback(i->asString());
    }

    unique_ptr<Response> request(string method, string uri, map<string, string> headersMap, slice body,
                                 HTTPStatus expectedStatus) {
        Encoder enc;
        enc.beginDict();
        for ( auto& h : headersMap ) {
            enc.writeKey(h.first);
            enc.writeString(h.second);
        }
        enc.endDict();
        auto headers = enc.finishDoc();

        share(db, "db"_sl);

        C4Log("---- %s %s", method.c_str(), uri.c_str());
        string               scheme = config.tlsConfig ? "https" : "http";
        auto                 port   = c4listener_getPort(listener());
        unique_ptr<Response> r(new Response(scheme, method, requestHostname, port, uri));
        r->setHeaders(headers).setBody(body);
        if ( pinnedCert ) r->allowOnlyCert(pinnedCert);
#    ifdef COUCHBASE_ENTERPRISE
        if ( rootCerts ) r->setRootCerts(rootCerts);
        if ( clientIdentity.cert ) r->setIdentity(clientIdentity.cert, clientIdentity.key);
#    endif
        if ( !r->run() ) C4LogToAt(kC4DefaultLog, kC4LogWarning, "Error: %s", c4error_descriptionStr(r->error()));
        C4Log("Status: %d %s", static_cast<int>(r->status()), r->statusMessage().c_str());
        string responseBody = r->body().asString();
        C4Log("Body: %s", responseBody.c_str());
        INFO("Error: " << c4error_descriptionStr(r->error()));
        REQUIRE(r->status() == expectedStatus);
        return r;
    }

    unique_ptr<Response> request(string method, string uri, HTTPStatus expectedStatus) {
        return request(method, uri, {}, nullslice, expectedStatus);
    }

    bool wait(std::shared_ptr<Response> response, C4ReplicatorActivityLevel activityLevel, unsigned timeoutSeconds) {
        Dict           b                   = response->bodyAsJSON().asDict();
        unsigned       sid                 = (unsigned)b.get("session_id").asUnsigned();
        const unsigned pollIntervalSeconds = 1;
        unsigned       nextTime            = 0;
        while ( timeoutSeconds == 0 || nextTime < timeoutSeconds ) {
            nextTime += pollIntervalSeconds;
            std::this_thread::sleep_for(chrono::seconds(pollIntervalSeconds));
            auto  r    = request("GET", "/_active_tasks", {{"Content-Type", "application/json"}}, "", HTTPStatus::OK);
            Array body = r->bodyAsJSON().asArray();
            Dict  sess;
            for ( Array::iterator iter(body); iter; ++iter ) {
                Dict     dict = iter.value().asDict();
                unsigned s    = (unsigned)dict.get("session_id").asUnsigned();
                if ( s == sid ) {
                    sess = dict;
                    break;
                }
            }
            if ( !sess ) { break; }
            static slice const kStatusName[] = {"Stopped"_sl, "Offline"_sl, "Connecting"_sl, "Idle"_sl, "Active"_sl};
            if ( sess.get("status").asString() == kStatusName[activityLevel] ) { return true; }
        }
        return false;
    }

    void testRootLevel() {
        auto r    = request("GET", "/", HTTPStatus::OK);
        auto body = r->bodyAsJSON().asDict();
        REQUIRE(body);
        CHECK(to_str(body["couchdb"]) == "Welcome");
    }

    alloc_slice directory;
    string      requestHostname{"localhost"};

    C4Listener*           receivedHTTPAuthFromListener = nullptr;
    optional<alloc_slice> receivedHTTPAuthHeader;
    bool                  allowHTTPConnection = true;

    alloc_slice pinnedCert;
#    ifdef COUCHBASE_ENTERPRISE
    c4::ref<C4Cert> rootCerts;

    C4Listener*           receivedCertAuthFromListener = nullptr;
    optional<alloc_slice> receivedCertAuth;
    bool                  allowClientCert = true;
#    endif
};

#    pragma mark - ROOT LEVEL:

TEST_CASE_METHOD(C4RESTTest, "Network interfaces", "[Listener][C]") {
    vector<string> interfaces;
    for ( auto& i : Interface::all() ) interfaces.push_back(i.name);
    vector<string> addresses;
    for ( auto& addr : Interface::allAddresses() ) addresses.push_back(string(addr));
    vector<string> primaryAddresses;
    for ( auto& addr : Interface::primaryAddresses() ) primaryAddresses.push_back(string(addr));
    auto hostname = GetMyHostName();
    C4Log("Interface names = {%s}", join(interfaces, ", ").c_str());
    C4Log("IP addresses =    {%s}", join(addresses, ", ").c_str());
    C4Log("Primary addrs =   {%s}", join(primaryAddresses, ", ").c_str());
    C4Log("Hostname =        %s", (hostname ? hostname->c_str() : "(unknown)"));
    CHECK(!interfaces.empty());
    CHECK(!primaryAddresses.empty());
    CHECK(!addresses.empty());
}

TEST_CASE_METHOD(C4RESTTest, "Listener URLs", "[Listener][C]") {
    share(db, "db"_sl);
    auto   configPortStr  = to_string(c4listener_getPort(listener()));
    string expectedSuffix = string(":") + configPortStr + "/";
    forEachURL(nullptr, kC4RESTAPI, [&expectedSuffix](string_view url) {
        C4Log("Listener URL = <%.*s>", SPLAT(slice(url)));
        CHECK(hasPrefix(url, "http://"));
        CHECK(hasSuffix(url, expectedSuffix));
    });
    forEachURL(db, kC4RESTAPI, [&expectedSuffix](string_view url) {
        C4Log("Database URL = <%.*s>", SPLAT(slice(url)));
        CHECK(hasPrefix(url, "http://"));
        CHECK(hasSuffix(url, expectedSuffix + "db"));
    });

    {
        ExpectingExceptions x;
        C4Error             err;
        FLMutableArray      invalid = c4listener_getURLs(listener(), db, kC4SyncAPI, &err);
        CHECK(!invalid);
        CHECK(err.domain == LiteCoreDomain);
        CHECK(err.code == kC4ErrorInvalidParameter);
    }
}

TEST_CASE_METHOD(C4RESTTest, "Listen on interface", "[Listener][C]") {
    optional<Interface> intf;
    string              intfAddress;
    SECTION("All interfaces") {
        C4Log("Here are all the IP interfaces and their addresses:");
        for ( auto& i : Interface::all() ) {
            C4Log("  - %s (%.02x, routable=%d) :", i.name.c_str(), i.flags, i.isRoutable());
            for ( auto& addr : i.addresses ) C4Log("    - %s", string(addr).c_str());
        }
    }
    SECTION("Specific interface") {
        intf        = Interface::all()[0];
        intfAddress = string(intf->addresses[0]);
        SECTION("Use interface name") {
            C4Log("Will listen on interface %s", intf->name.c_str());
            config.networkInterface = slice(intf->name);
        }
        SECTION("Use interface address") {
            C4Log("Will listen on address %s", intfAddress.c_str());
            config.networkInterface = slice(intfAddress);
        }
    }

    share(db, "db"_sl);

    // Check that the listener's reported URLs contain the interface address:
    forEachURL(db, kC4RESTAPI, [&](string_view url) {
        C4Log("Checking URL <%.*s>", SPLAT(slice(url)));
        C4Address address;
        C4String  dbName;
        INFO("URL is <" << url << ">");
        CHECK(c4address_fromURL(slice(url), &address, &dbName));
        CHECK(address.port == c4listener_getPort(listener()));
        CHECK(dbName == "db"_sl);

        if ( intf ) {
            requestHostname           = string(slice(address.hostname));
            bool foundAddrInInterface = false;
            bool addrIsIPv6           = false;
            for ( auto& addr : intf->addresses ) {
                if ( string(addr) == requestHostname ) {
                    foundAddrInInterface = true;
                    addrIsIPv6           = addr.isIPv6();
                    break;
                }
            }
            CHECK(foundAddrInInterface);
        }

        testRootLevel();
    });
}

TEST_CASE_METHOD(C4RESTTest, "Listener Auto-Select Port", "[Listener][C]") {
    share(db, "db"_sl);
    const auto port = c4listener_getPort(listener());
    C4Log("System selected port %u", port);
    CHECK(port != 0);
}

TEST_CASE_METHOD(C4RESTTest, "No Listeners on Same Port", "[Listener][C]") {
    share(db, "db"_sl);
    config.port = c4listener_getPort(listener());
    C4Error err;

    ExpectingExceptions x;
    auto                listener2 = c4listener_start(&config, &err);
    CHECK(!listener2);
    CHECK(err.domain == POSIXDomain);
    CHECK(err.code == EADDRINUSE);
}

TEST_CASE_METHOD(C4RESTTest, "REST root level", "[REST][Listener][C]") { testRootLevel(); }

TEST_CASE_METHOD(C4RESTTest, "REST _all_databases", "[REST][Listener][C]") {
    auto r    = request("GET", "/_all_dbs", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asArray();
    REQUIRE(body.count() == 1);
    CHECK(to_str(body[0]) == "db");
}

TEST_CASE_METHOD(C4RESTTest, "REST unknown special top-level", "[REST][Listener][C]") {
    request("GET", "/_foo", HTTPStatus::NotFound);
    request("GET", "/_", HTTPStatus::NotFound);
}

#    pragma mark - DATABASE:

TEST_CASE_METHOD(C4RESTTest, "REST GET database", "[REST][Listener][C]") {
    unique_ptr<Response> r;
    SECTION("No slash") { r = request("GET", "/db", HTTPStatus::OK); }
    SECTION("URL-encoded") { r = request("GET", "/%64%62", HTTPStatus::OK); }
    SECTION("With slash") { r = request("GET", "/db/", HTTPStatus::OK); }
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    CHECK(to_str(body["db_name"]) == "db");
    CHECK(to_str(body["collection_name"]) == "_default");
    CHECK(to_str(body["scope_name"]) == "_default");
    CHECK(body["db_uuid"].type() == kFLString);
    CHECK(body["db_uuid"].asString().size >= 32);
    CHECK(body["doc_count"].type() == kFLNumber);
    CHECK(body["doc_count"].asInt() == 0);
    CHECK(body["update_seq"].type() == kFLNumber);
    CHECK(body["update_seq"].asInt() == 0);
}

TEST_CASE_METHOD(C4RESTTest, "REST DELETE database", "[REST][Listener][C]") {
    unique_ptr<Response> r;
    SECTION("Disallowed") { r = request("DELETE", "/db", HTTPStatus::Forbidden); }
    SECTION("Allowed") {
        config.allowDeleteDBs = true;
        r                     = request("DELETE", "/db", HTTPStatus::OK);
        r                     = request("GET", "/db", HTTPStatus::NotFound);
        REQUIRE(!FilePath(string(databasePath())).exists());
        // This is the easiest cross-platform way to check that the db was deleted:
        //REQUIRE(remove(string(databasePath()).c_str()) != 0);
        REQUIRE(errno == ENOENT);
    }
}

TEST_CASE_METHOD(C4RESTTest, "REST PUT database", "[REST][Listener][C]") {
    unique_ptr<Response> r;
    SECTION("Disallowed") {
        r = request("PUT", "/db", HTTPStatus::Forbidden);
        r = request("PUT", "/otherdb", HTTPStatus::Forbidden);
        //        r = request("PUT", "/and%2For", HTTPStatus::Forbidden);       // that's a slash. This is a legal db name.
    }
    SECTION("Allowed") {
        setUpDirectory();
        SECTION("Duplicate") { r = request("PUT", "/db", HTTPStatus::PreconditionFailed); }
        SECTION("New DB") {
            r = request("PUT", "/otherdb", HTTPStatus::Created);
            r = request("GET", "/otherdb", HTTPStatus::OK);

            SECTION("Test _all_dbs again") {
                r         = request("GET", "/_all_dbs", HTTPStatus::OK);
                auto body = r->bodyAsJSON().asArray();
                REQUIRE(body.count() == 2);
                CHECK(to_str(body[0]) == "db");
                CHECK(to_str(body[1]) == "otherdb");
            }
        }
    }
}

#    pragma mark - COLLECTIONS:

TEST_CASE_METHOD(C4RESTTest, "REST GET database with collections", "[REST][Listener][C]") {
    REQUIRE(c4db_createCollection(db, {"guitars"_sl, "stuff"_sl}, ERROR_INFO()));
    REQUIRE(c4db_createCollection(db, {"synths"_sl, "stuff"_sl}, ERROR_INFO()));

    unique_ptr<Response> r;
    r         = request("GET", "/db", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    Log("%.*s", SPLAT(r->body()));
    Dict scopes = body["scopes"].asDict();
    REQUIRE(scopes);
    CHECK(scopes.count() == 2);
    Dict stuff = scopes["stuff"].asDict();
    REQUIRE(stuff);
    Dict guitars = stuff["guitars"].asDict();
    REQUIRE(guitars);
    CHECK(guitars["doc_count"].type() == kFLNumber);
    CHECK(guitars["doc_count"].asInt() == 0);
    CHECK(guitars["update_seq"].type() == kFLNumber);
    CHECK(guitars["update_seq"].asInt() == 0);
    CHECK(stuff["synths"].asDict());

    Dict dflt = scopes["_default"].asDict();
    REQUIRE(dflt);
    CHECK(dflt.count() == 1);
    Dict dfltColl = dflt["_default"].asDict();
    REQUIRE(dfltColl);
    CHECK(dfltColl["doc_count"].type() == kFLNumber);
    CHECK(dfltColl["doc_count"].asInt() == 0);
    CHECK(dfltColl["update_seq"].type() == kFLNumber);
    CHECK(dfltColl["update_seq"].asInt() == 0);
}

TEST_CASE_METHOD(C4RESTTest, "REST GET collection", "[REST][Listener][C]") {
    REQUIRE(c4db_createCollection(db, {"guitars"_sl, "stuff"_sl}, ERROR_INFO()));

    unique_ptr<Response> r;
    r = request("GET", "/db.guitars/", HTTPStatus::NotFound);

    {
        ExpectingExceptions x;
        r = request("GET", "/./", HTTPStatus::BadRequest);
        r = request("GET", "/db./", HTTPStatus::BadRequest);
        r = request("GET", "/.db./", HTTPStatus::BadRequest);
        r = request("GET", "/db../", HTTPStatus::BadRequest);
        r = request("GET", "/db.stuff.guitars./", HTTPStatus::BadRequest);
    }

    r = request("GET", "/db.foo/", HTTPStatus::NotFound);
    r = request("GET", "/db.foo.bar/", HTTPStatus::NotFound);

    r         = request("GET", "/db.stuff.guitars/", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asDict();
    REQUIRE(body);
    CHECK(to_str(body["db_name"]) == "db");
    CHECK(to_str(body["collection_name"]) == "guitars");
    CHECK(to_str(body["scope_name"]) == "stuff");
    CHECK(body["db_uuid"].type() == kFLString);
    CHECK(body["db_uuid"].asString().size >= 32);
    CHECK(body["doc_count"].type() == kFLNumber);
    CHECK(body["doc_count"].asInt() == 0);
    CHECK(body["update_seq"].type() == kFLNumber);
    CHECK(body["update_seq"].asInt() == 0);
}

TEST_CASE_METHOD(C4RESTTest, "REST DELETE collection", "[REST][Listener][C]") {
    REQUIRE(c4db_createCollection(db, {"guitars"_sl, "stuff"_sl}, ERROR_INFO()));

    unique_ptr<Response> r;
    SECTION("Disallowed") { r = request("DELETE", "/db.stuff.guitars", HTTPStatus::Forbidden); }
    SECTION("Allowed") {
        config.allowDeleteCollections = true;
        r                             = request("DELETE", "/db.stuff.guitars", HTTPStatus::OK);
        r                             = request("GET", "/db.stuff.guitars", HTTPStatus::NotFound);
    }
}

TEST_CASE_METHOD(C4RESTTest, "REST PUT collection", "[REST][Listener][C]") {
    unique_ptr<Response> r;
    SECTION("Disallowed") {
        r = request("PUT", "/db.foo", HTTPStatus::Forbidden);
        r = request("PUT", "/db.foo.bar", HTTPStatus::Forbidden);
    }
    SECTION("Allowed") {
        config.allowCreateCollections = true;
        SECTION("Duplicate") { r = request("PUT", "/db._default._default", HTTPStatus::PreconditionFailed); }
        SECTION("New Collection") {
            r = request("PUT", "/db.guitars", HTTPStatus::Created);
            r = request("GET", "/db.guitars", HTTPStatus::OK);
        }
    }
}

#    pragma mark - DOCUMENTS:

TEST_CASE_METHOD(C4RESTTest, "REST CRUD", "[REST][Listener][C]") {
    unique_ptr<Response> r;
    Dict                 body;
    alloc_slice          docID;

    string        dbPath;
    C4Collection* coll;
    bool          inCollection = GENERATE(false, true);
    if ( inCollection ) {
        Log("---- Using collection 'coll'");
        coll = c4db_createCollection(db, {"coll"_sl}, ERROR_INFO());
        REQUIRE(coll);
        dbPath = "/db.coll";
    } else {
        coll   = c4db_getDefaultCollection(db, nullptr);
        dbPath = "/db";
    }

    SECTION("POST") {
        r = request("POST", dbPath, {{"Content-Type", "application/json"}}, "{\"year\": 1964}"_sl, HTTPStatus::Created);
        body  = r->bodyAsJSON().asDict();
        docID = body["id"].asString();
        CHECK(docID.size >= 20);
    }

    SECTION("PUT") {
        r     = request("PUT", dbPath + "/mydocument", {{"Content-Type", "application/json"}}, "{\"year\": 1964}"_sl,
                        HTTPStatus::Created);
        body  = r->bodyAsJSON().asDict();
        docID = body["id"].asString();
        CHECK(docID == "mydocument"_sl);

        request("PUT", dbPath + "/mydocument", {{"Content-Type", "application/json"}}, "{\"year\": 1977}"_sl,
                HTTPStatus::Conflict);
        request("PUT", dbPath + "/mydocument", {{"Content-Type", "application/json"}},
                "{\"year\": 1977, \"_rev\":\"1-ffff\"}"_sl, HTTPStatus::Conflict);
    }

    CHECK(body["ok"].asBool() == true);
    alloc_slice revID(body["rev"].asString());
    CHECK(revID.size > 0);

    {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, docID, true, kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        CHECK(doc->revID == revID);
        body = c4doc_getProperties(doc);
        CHECK(body["year"].asInt() == 1964);
        CHECK(body.count() == 1);  // i.e. no _id or _rev properties
    }

    r    = request("GET", dbPath + "/" + docID.asString(), HTTPStatus::OK);
    body = r->bodyAsJSON().asDict();
    CHECK(body["_id"].asString() == docID);
    CHECK(body["_rev"].asString() == revID);
    CHECK(body["year"].asInt() == 1964);

    r    = request("DELETE", dbPath + "/" + docID.asString() + "?rev=" + revID.asString(), HTTPStatus::OK);
    body = r->bodyAsJSON().asDict();
    CHECK(body["ok"].asBool() == true);
    revID = body["rev"].asString();

    {
        c4::ref<C4Document> doc = c4coll_getDoc(coll, docID, true, kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        CHECK((doc->flags & kDocDeleted) != 0);
        CHECK(doc->revID == revID);
        body = c4doc_getProperties(doc);
        CHECK(body.count() == 0);
    }

    r = request("GET", dbPath + "/" + docID.asString(), HTTPStatus::NotFound);
}

TEST_CASE_METHOD(C4RESTTest, "REST _all_docs", "[REST][Listener][C]") {
    auto r    = request("GET", "/db/_all_docs", HTTPStatus::OK);
    auto body = r->bodyAsJSON().asDict();
    auto rows = body["rows"].asArray();
    CHECK(rows);
    CHECK(rows.count() == 0);

    request("PUT", "/db/mydocument", {{"Content-Type", "application/json"}}, "{\"year\": 1964}"_sl,
            HTTPStatus::Created);
    request("PUT", "/db/foo", {{"Content-Type", "application/json"}}, "{\"age\": 17}"_sl, HTTPStatus::Created);

    r    = request("GET", "/db/_all_docs", HTTPStatus::OK);
    body = r->bodyAsJSON().asDict();
    rows = body["rows"].asArray();
    CHECK(rows);
    CHECK(rows.count() == 2);
    auto row = rows[0].asDict();
    CHECK(row["key"].asString() == "foo"_sl);
    row = rows[1].asDict();
    CHECK(row["key"].asString() == "mydocument"_sl);
}

TEST_CASE_METHOD(C4RESTTest, "REST _bulk_docs", "[REST][Listener][C]") {
    unique_ptr<Response> r;
    r          = request("POST", "/db/_bulk_docs", {{"Content-Type", "application/json"}},
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

#    pragma mark - HTTP AUTH:

TEST_CASE_METHOD(C4RESTTest, "REST HTTP auth missing", "[REST][Listener][C]") {
    setupHTTPAuth();
    allowHTTPConnection = false;
    auto r              = request("GET", "/", HTTPStatus::Unauthorized);
    CHECK(r->header("WWW-Authenticate") == "Basic charset=\"UTF-8\"");
    CHECK(receivedHTTPAuthFromListener == listener());
    CHECK(receivedHTTPAuthHeader == nullslice);
}

TEST_CASE_METHOD(C4RESTTest, "REST HTTP auth incorrect", "[REST][Listener][C]") {
    setupHTTPAuth();
    allowHTTPConnection = false;
    auto r              = request("GET", "/", {{"Authorization", "Basic xxxx"}}, nullslice, HTTPStatus::Unauthorized);
    CHECK(r->header("WWW-Authenticate") == "Basic charset=\"UTF-8\"");
    CHECK(receivedHTTPAuthFromListener == listener());
    CHECK(receivedHTTPAuthHeader == "Basic xxxx");
}

TEST_CASE_METHOD(C4RESTTest, "REST HTTP auth correct", "[REST][Listener][C]") {
    setupHTTPAuth();
    allowHTTPConnection = true;
    auto r              = request("GET", "/", {{"Authorization", "Basic xxxx"}}, nullslice, HTTPStatus::OK);
    CHECK(receivedHTTPAuthFromListener == listener());
    CHECK(receivedHTTPAuthHeader == "Basic xxxx");
}

#    pragma mark - TLS:


#    ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(C4RESTTest, "TLS REST URLs", "[REST][Listener][C]") {
    useServerTLSWithTemporaryKey();
    share(db, "db"_sl);
    auto   configPortStr  = to_string(c4listener_getPort(listener()));
    string expectedSuffix = string(":") + configPortStr + "/";
    forEachURL(nullptr, kC4RESTAPI, [&expectedSuffix](string_view url) {
        C4Log("Listener URL = <%.*s>", SPLAT(slice(url)));
        CHECK(hasPrefix(url, "https://"));
        CHECK(hasSuffix(url, expectedSuffix));
    });
    forEachURL(db, kC4RESTAPI, [&expectedSuffix](string_view url) {
        C4Log("Database URL = <%.*s>", SPLAT(slice(url)));
        CHECK(hasPrefix(url, "https://"));
        CHECK(hasSuffix(url, expectedSuffix + "db"));
    });

    {
        ExpectingExceptions x;
        C4Error             err;
        FLMutableArray      invalid = c4listener_getURLs(listener(), db, kC4SyncAPI, &err);
        CHECK(!invalid);
        CHECK(err.domain == LiteCoreDomain);
        CHECK(err.code == kC4ErrorInvalidParameter);
    }
}

TEST_CASE_METHOD(C4RESTTest, "TLS REST untrusted cert", "[REST][Listener][TLS][C]") {
    useServerTLSWithTemporaryKey();

    gC4ExpectExceptions = true;
    auto r              = request("GET", "/", HTTPStatus::undefined);
    CHECK(r->error() == (C4Error{NetworkDomain, kC4NetErrTLSCertUnknownRoot}));
}

TEST_CASE_METHOD(C4RESTTest, "TLS REST pinned cert", "[REST][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithTemporaryKey();
    testRootLevel();
}


#        ifdef PERSISTENT_PRIVATE_KEY_AVAILABLE
TEST_CASE_METHOD(C4RESTTest, "TLS REST pinned cert persistent key", "[REST][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithPersistentKey();
    testRootLevel();
}
#        endif


TEST_CASE_METHOD(C4RESTTest, "TLS REST client cert", "[REST][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithTemporaryKey();
    useClientTLSWithTemporaryKey();
    testRootLevel();
}

TEST_CASE_METHOD(C4RESTTest, "TLS REST client cert w/auth callback", "[REST][Listener][TLS][C]") {
    pinnedCert = useServerTLSWithTemporaryKey();
    useClientTLSWithTemporaryKey();

    setupCertAuth();
    config.tlsConfig->requireClientCerts = true;
    allowClientCert                      = false;

    auto r = request("GET", "/", HTTPStatus::undefined);
    CHECK(r->error() == (C4Error{NetworkDomain, kC4NetErrTLSCertRejectedByPeer}));
}

TEST_CASE_METHOD(C4RESTTest, "TLS REST cert chain", "[REST][Listener][TLS][C]") {
    Identity ca = CertHelper::createIdentity(false, kC4CertUsage_TLS_CA, "Test CA", nullptr, nullptr, true);
    useServerIdentity(CertHelper::createIdentity(false, kC4CertUsage_TLSServer, "localhost", nullptr, &ca));
    auto summary = alloc_slice(c4cert_summary(serverIdentity.cert));
    useClientIdentity(CertHelper::createIdentity(false, kC4CertUsage_TLSClient, "Test Client", nullptr, &ca));
    setListenerRootClientCerts(ca.cert);
    rootCerts = c4cert_retain(ca.cert);
    testRootLevel();
}

TEST_CASE_METHOD(C4RESTTest, "Sync Listener URLs", "[REST][Listener][TLS][C]") {
    bool   expectErrorForREST = false;
    string restScheme         = "http";
    string syncScheme         = "ws";

    config.allowPull = true;
    config.allowPush = true;
    SECTION("Plain") {
        SECTION("With REST") { config.apis = kC4RESTAPI | kC4SyncAPI; }

        SECTION("Without REST") {
            expectErrorForREST = true;
            config.apis        = kC4SyncAPI;
        }
    }

    SECTION("TLS") {
        useServerTLSWithTemporaryKey();
        syncScheme = "wss";
        SECTION("With REST") {
            restScheme  = "https";
            config.apis = kC4RESTAPI | kC4SyncAPI;
        }

        SECTION("Without REST") {
            expectErrorForREST = true;
            config.apis        = kC4SyncAPI;
        }
    }

    share(db, "db");
    auto   configPortStr  = to_string(c4listener_getPort(listener()));
    string expectedSuffix = string(":") + configPortStr + "/";
    if ( expectErrorForREST ) {
        C4Error             err;
        ExpectingExceptions e;
        FLMutableArray      invalid = c4listener_getURLs(listener(), db, kC4RESTAPI, &err);
        CHECK(!invalid);
        CHECK(err.domain == LiteCoreDomain);
        CHECK(err.code == kC4ErrorInvalidParameter);
    } else {
        forEachURL(db, kC4RESTAPI, [&expectedSuffix, &restScheme](string_view url) {
            C4Log("Database URL = <%.*s>", SPLAT(slice(url)));
            CHECK(hasPrefix(url, restScheme));
            CHECK(hasSuffix(url, expectedSuffix + "db"));
        });
    }

    forEachURL(db, kC4SyncAPI, [&expectedSuffix, &syncScheme](string_view url) {
        C4Log("Database URL = <%.*s>", SPLAT(slice(url)));
        CHECK(hasPrefix(url, syncScheme));
        CHECK(hasSuffix(url, expectedSuffix + "db"));
    });
}

// Following test cases that are tagged with [.SyncServer] are not exercised in the automatic build.
// They require special set-up of the Sync Gateway server, c.f. ReplicatorSGTest.cc.

// Continuous replication without authentication
TEST_CASE_METHOD(C4RESTTest, "REST HTTP Replicate, Continuous", "[REST][Listener][C][.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    std::stringstream body;
    string            targetDb = slice(ReplicatorAPITest::kScratchDBName).asString();
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << targetDb << "',"
         << "continuous: true}";
    shared_ptr<Response> r =
            request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(wait(r, kC4Idle, 5));

    std::stringstream newss;
    body.swap(newss);
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << targetDb << "',"
         << "cancel: true}";
    request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(wait(r, kC4Stopped, 5));
}

// OneShot replication.
TEST_CASE_METHOD(C4RESTTest, "REST HTTP Replicate, OneShot", "[REST][Listener][C][.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    std::stringstream body;
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << slice(ReplicatorAPITest::kScratchDBName).asString() << "',"
         << "continuous: false}";

    auto r = request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(r->status() == HTTPStatus::OK);
}

// Continuous replication with authentication
TEST_CASE_METHOD(C4RESTTest, "REST HTTP Replicate Continuous, Auth", "[REST][Listener][C][.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    std::stringstream body;
    string            targetDb = slice(ReplicatorAPITest::kProtectedDBName).asString();
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << targetDb << "',"
         << "user: 'pupshaw',"
         << "password: 'frank',"
         << "continuous: true}";
    shared_ptr<Response> r =
            request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(wait(r, kC4Idle, 5));

    std::stringstream newss;
    body.swap(newss);
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << targetDb << "',"
         << "cancel: true}";
    request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(wait(r, kC4Stopped, 5));
}

// This test uses SG set up for [.SyncServerCollection], but without TLS. c.f ReplicatorCollectionSG.cc.
TEST_CASE_METHOD(C4RESTTest, "REST HTTP Replicate Continuous (collections)",
                 "[REST][Listener][C][.SyncServerCollection]") {
    constexpr C4CollectionSpec   Roses  = {"roses"_sl, "flowers"_sl};
    constexpr C4CollectionSpec   Tulips = {"tulips"_sl, "flowers"_sl};
    std::array<C4Collection*, 2> collections;
    collections[0]  = db->createCollection(Roses);
    collections[1]  = db->createCollection(Tulips);
    string idPrefix = timePrefix();
    for ( auto& coll : collections ) { importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 0, idPrefix); }

    std::stringstream body;
    string            targetDb = "scratch";
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << targetDb << "',"
         << "user: 'pupshaw',"
         << "password: 'frank',"
         << "continuous: true,"
         << "collections: ['flowers.roses','flowers.tulips']}";

    shared_ptr<Response> r =
            request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(wait(r, kC4Idle, 5));

    std::stringstream newss;
    body.swap(newss);
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << targetDb << "',"
         << "cancel: true}";
    request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(wait(r, kC4Stopped, 5));
}

// OneShot replication with authentication.
TEST_CASE_METHOD(C4RESTTest, "REST HTTP Replicate Oneshot, Auth", "[REST][Listener][C][.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    std::stringstream body;
    body << "{source: 'db',"
         << "target: 'ws://localhost:4984/" << slice(ReplicatorAPITest::kProtectedDBName).asString() << "',"
         << "user: 'pupshaw',"
         << "password: 'frank',"
            "continuous: false}";

    auto r = request("POST", "/_replicate", {{"Content-Type", "application/json"}}, json5(body.str()), HTTPStatus::OK);
    CHECK(r->status() == HTTPStatus::OK);
}

#    endif  // COUCHBASE_ENTERPRISE

#endif
