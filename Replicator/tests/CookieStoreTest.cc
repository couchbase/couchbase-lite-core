//
//  CookieStoreTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 6/8/17.
//Copyright © 2017 Couchbase. All rights reserved.
//

#include "LiteCoreTest.hh"
#include "CookieStore.hh"

using namespace litecore::repl;
using namespace litecore::websocket;
using namespace std;


TEST_CASE("Cookie Parser", "[cookies]") {
    SECTION("Minimal") {
        Cookie c("name=", "example.com");
        CHECK(c);
        CHECK(c.name == "name");
        CHECK(c.value == "");
        CHECK(c.domain == "example.com");
        CHECK(c.path == "");
        CHECK(!c.secure);
        CHECK(!c.persistent());
        CHECK(!c.expired());
    }
    SECTION("Basic") {
        Cookie c("name=value", "example.com");
        CHECK(c);
        CHECK(c.name == "name");
        CHECK(c.value == "value");
        CHECK(c.domain == "example.com");
        CHECK(c.path == "");
        CHECK(!c.secure);
        CHECK(!c.persistent());
        CHECK(!c.expired());
    }
    SECTION("Quoted value") {
        Cookie c("size=\"XXL\"", "example.com");
        CHECK(c);
        CHECK(c.name == "size");
        CHECK(c.value == "XXL");
    }
    SECTION("Domain") {
        Cookie c("x=y; Domain=example.com", "example.com");
        CHECK(c);
        CHECK(c.domain == "example.com");
    }
    SECTION("Subdomain") {
        Cookie c("x=y; Domain=www.example.com", "example.com");
        CHECK(c);
        CHECK(c.domain == "www.example.com");
    }
    SECTION("Subdomain Case-Insensitive") {
        Cookie c("x=y; Domain=WWW.Example.Com", "example.com");
        CHECK(c);
        CHECK(c.domain == "WWW.Example.Com");
    }
    SECTION("Path") {
        Cookie c("x=y; Path=/foo/bar", "example.com");
        CHECK(c);
        CHECK(c.path == "/foo/bar");
    }
    SECTION("Secure") {
        Cookie c("x=y; Path=/foo/bar; Secure=", "example.com");
        CHECK(c);
        CHECK(c.secure);
    }
    SECTION("Expires") {
        Cookie c("x=y; lang=en-US; Expires=Wed, 09 Jun 2099 10:18:14 GMT", "example.com");
        CHECK(c);
        CHECK(c.name == "x");
        CHECK(c.value == "y");
        CHECK(c.expires == 4084654694);
        CHECK(c.domain == "example.com");
        CHECK(c.path == "");
        CHECK(c.persistent());
        CHECK(!c.expired());        // This check will fail starting in 2099...
    }
    SECTION("Expired") {
        Cookie c("x=y; lang=en-US; Expires=Wed, 09 Jun 1999 10:18:14 GMT", "example.com");
        CHECK(c);
        CHECK(c.name == "x");
        CHECK(c.value == "y");
        CHECK(c.expires == 928898294);
        CHECK(c.domain == "example.com");
        CHECK(c.path == "");
        CHECK(c.persistent());
        CHECK(c.expired());
    }
    SECTION("Max-Age") {
        Cookie c("x=y; lang=en-US; Max-Age=30", "example.com");
        CHECK(c);
        CHECK(c.name == "x");
        CHECK(c.value == "y");
        CHECK(abs(c.expires - (time(NULL) + 30)) <= 1);
        CHECK(c.domain == "example.com");
        CHECK(c.path == "");
        CHECK(c.persistent());
        CHECK(!c.expired());
    }
}


TEST_CASE("Cookie Parser Failure", "[cookies]") {
    static const char* badCookies[] = {
        "",
        "duh?",
        "=value",
        "name=value; Domain=counterexample.com",
        "name=value; Domain=couchbase.com",
        "name=value; Domain=.com",
        "name=value; Domain=",
        "name=value; Expires=someday",
        "name=value; Max-Age=123x3",
        "name=value; Max-Age=z7",
        "name=value; Max-Age=",
    };
    for (int i = 0; i < sizeof(badCookies)/sizeof(badCookies[0]); ++i) {
        INFO("Checking " << badCookies[i]);
        Cookie c(badCookies[i], "example.com");
        CHECK(!c);
    }
}


static const Address kRequest           {"blip", "www.example.com", 4984, "/db/_blipsync"};
static const Address kSecureRequest     {"blips", "www.example.com", 4984, "/db/_blipsync"};
static const Address kOtherPathRequest  {"blips", "www.example.com", 4984, "/qat/_blipsync"};
static const Address kOtherHostRequest  {"blip", "couchbase.com", 4984, "/beer/_blipsync"};


TEST_CASE("CookieStore", "[Cookies]") {
    CookieStore store;
    CHECK(store.cookies().empty());
    CHECK(!store.changed());
    CHECK(store.cookiesForRequest(kRequest).empty());

    CHECK(store.setCookie("x=y; Domain=Example.Com", "example.com"));
    CHECK(!store.cookies().empty());
    CHECK(!store.changed());    // it's non-persistent
    CHECK(store.setCookie("e=mc^2; Domain=WWW.Example.Com; Max-Age=30", "www.example.com"));
    CHECK(store.changed());
    CHECK(store.setCookie("jens=awesome; Domain=snej.example.com", "example.com"));
    CHECK(store.cookiesForRequest(kRequest) == "x=y; e=mc^2");
    CHECK(store.cookiesForRequest(kOtherPathRequest) == "x=y; e=mc^2");
    CHECK(store.cookiesForRequest(kSecureRequest) == "x=y; e=mc^2");
    CHECK(store.cookiesForRequest(kOtherHostRequest) == "");

    SECTION("Replace Cookie") {
        store.clearChanged();
        CHECK(store.setCookie("e=something else; Domain=WWW.Example.Com", "www.example.com"));
        CHECK(store.changed());     // a persistent cookie got removed
        CHECK(store.cookiesForRequest(kRequest) == "x=y; e=something else");
    }
    SECTION("Secure Cookie") {
        CHECK(store.setCookie("password=123456; Domain=WWW.Example.Com; Secure=true", "www.example.com"));
        CHECK(store.cookiesForRequest(kRequest) == "x=y; e=mc^2");
        CHECK(store.cookiesForRequest(kSecureRequest) == "x=y; e=mc^2; password=123456");
    }
    SECTION("Paths") {
        CHECK(store.setCookie("path=qat; Domain=example.com; Path=/qat", "example.com"));
        CHECK(store.setCookie("path=Qat; Domain=example.com; Path=/Qat", "example.com"));
        CHECK(store.setCookie("path=qaternion; Domain=example.com; Path=/qaternion", "example.com"));
        CHECK(store.setCookie("x=z; Domain=Example.com; Path=/elsewhere", "example.com"));
        CHECK(store.cookiesForRequest(kRequest) == "x=y; e=mc^2");
        CHECK(store.cookiesForRequest(kOtherPathRequest) == "x=y; e=mc^2; path=qat");
    }
    SECTION("Persistence") {
        alloc_slice encoded = store.encode();
        CHECK(encoded);
        CookieStore store2(encoded);
        CHECK(store2.cookies().size() == 1);
        CHECK(!store2.changed());
        CHECK(store2.cookiesForRequest(kRequest) == "e=mc^2");
    }
}

