//
//  CookieStoreTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 6/8/17.
//Copyright 2017-Present Couchbase, Inc.
//
//Use of this software is governed by the Business Source License included in
//the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
//file, in accordance with the Business Source License, use of this software
//will be governed by the Apache License, Version 2.0, included in the file
//licenses/APL2.txt.
//

#include "c4Test.hh"
#include "c4CppUtils.hh"
#include "c4Replicator.h"
#include "CookieStore.hh"
#include "DatabaseCookies.hh"
#include "Address.hh"

using namespace fleece;
using namespace litecore;
using namespace litecore::net;
using namespace litecore::repl;
using namespace std;

TEST_CASE("Cookie Parser", "[cookies]") {
    SECTION("Minimal") {
        Cookie c("name=", "example.com", "/");
        CHECK(c);
        CHECK(c.name == "name");
        CHECK(c.value.empty());
        CHECK(c.domain == "example.com");
        CHECK(c.path.empty());
        CHECK(!c.secure);
        CHECK(!c.persistent());
        CHECK(!c.expired());
    }
    SECTION("Basic") {
        Cookie c("name=value", "example.com", "/");
        CHECK(c);
        CHECK(c.name == "name");
        CHECK(c.value == "value");
        CHECK(c.domain == "example.com");
        CHECK(c.path.empty());
        CHECK(!c.secure);
        CHECK(!c.persistent());
        CHECK(!c.expired());
    }
    SECTION("Quoted value") {
        Cookie c("size=\"XXL\"", "example.com", "/");
        CHECK(c);
        CHECK(c.name == "size");
        CHECK(c.value == "XXL");
    }
    SECTION("Domain") {
        Cookie c("x=y; Domain=example.com", "example.com", "/");
        CHECK(c);
        CHECK(c.domain == "example.com");
        Cookie d("x=y; doMaIN=example.com", "example.com", "/");
        CHECK(d);
        CHECK(d.domain == "example.com");
    }
    SECTION("Subdomain") {
        Cookie c("x=y; Domain=www.example.com", "example.com", "/");
        CHECK(c);
        CHECK(c.domain == "www.example.com");
    }
    SECTION("Subdomain Case-Insensitive") {
        Cookie c("x=y; Domain=WWW.Example.Com", "example.com", "/");
        CHECK(c);
        CHECK(c.domain == "WWW.Example.Com");
    }
    SECTION("Subdomain Leading Dot") {
        Cookie c("x=y; Domain=.www.example.com", "example.com", "/");
        CHECK(c);
        CHECK(c.domain == "www.example.com");
    }
    SECTION("Implicit Path") {
        Cookie c("x=y", "example.com", "/db/_blipsync");
        CHECK(c);
        CHECK(c.path == "/db");
    }
    SECTION("Implicit Path 2") {
        Cookie c("x=y", "example.com", "/db/");
        CHECK(c);
        CHECK(c.path == "/db");
    }
    SECTION("Path") {
        Cookie c("x=y; Path=/foo/bar", "example.com", "/db/");
        CHECK(c);
        CHECK(c.path == "/foo/bar");
        Cookie d("x=y; patH=/foo/bar", "example.com", "/db/");
        CHECK(d);
        CHECK(d.path == "/foo/bar");
    }
    SECTION("Secure") {
        Cookie c("x=y; Path=/foo/bar; Secure=", "example.com", "/");
        CHECK(c);
        CHECK(c.secure);
        Cookie d("x=y; Path=/foo/bar; sEcure=", "example.com", "/");
        CHECK(d);
        CHECK(d.secure);
    }
    SECTION("Expires") {
        Cookie c("x=y; lang=en-US; EXPIRES=Tue, 09 Jun 2099 10:18:14 GMT", "example.com", "/");
        CHECK(c);
        CHECK(c.name == "x");
        CHECK(c.value == "y");
        if ( sizeof(time_t) == 4 ) {
            CHECK(c.expires == 2147483647);
        } else {
            CHECK(c.expires == 4084683494);
        }

        CHECK(c.domain == "example.com");
        CHECK(c.path.empty());
        CHECK(c.persistent());
        CHECK(!c.expired());  // This check will fail starting in 2099...
    }
    // CBL-3949
    SECTION("GCLB Cookie") {
        Cookie c("GCLB=COWjp4rwlqauaQ; path=/; HttpOnly; lang=en-US; EXPIRES=Tue, 09-Jun-2099 10:18:14 GMT",
                 "example.com", "/");
        CHECK(c);
        CHECK(c.name == "gclb");
        CHECK(c.value == "COWjp4rwlqauaQ");
        if ( sizeof(time_t) == 4 ) {
            CHECK(c.expires == 2147483647);
        } else {
            CHECK(c.expires == 4084683494);
        }
        CHECK(c.domain == "example.com");
        CHECK(c.path == "/");
        CHECK(c.persistent());
        CHECK(!c.expired());
    }
    SECTION("Expires - ANSI C format") {
        Cookie c("x=y; lang=en-US; expires=Tue Jun  9 10:18:14 2099", "example.com", "/");
        CHECK(c);
        if ( sizeof(time_t) == 4 ) {
            CHECK(c.expires == 2147483647);
        } else {
            CHECK(c.expires == 4084683494);
        }
    }
    SECTION("Expired") {
        Cookie c("x=y; lang=en-US; expires=Wed, 09 Jun 1999 10:18:14 GMT", "example.com", "/");
        CHECK(c);
        CHECK(c.name == "x");
        CHECK(c.value == "y");
        CHECK(c.expires == 928923494);
        CHECK(c.domain == "example.com");
        CHECK(c.path.empty());
        CHECK(c.persistent());
        CHECK(c.expired());
    }
    SECTION("Max-Age") {
        Cookie c("x=y; lang=en-US; Max-age=30", "example.com", "/");
        CHECK(c);
        CHECK(c.name == "x");
        CHECK(c.value == "y");
        CHECK(abs(c.expires - (time(nullptr) + 30)) <= 1);
        CHECK(c.domain == "example.com");
        CHECK(c.path.empty());
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
    for ( const auto& badCookie : badCookies ) {
        INFO("Checking " << badCookie);
        ExpectingExceptions x;
        Cookie              c(badCookie, "example.com", "/");
        CHECK(!c);
    }
}

static const C4Address kRequest{kC4Replicator2Scheme, "www.example.com"_sl, 4984, "/db/_blipsync"_sl};
static const C4Address kSecureRequest{kC4Replicator2TLSScheme, "www.example.com"_sl, 4984, "/db/_blipsync"_sl};
static const C4Address kOtherPathRequest{kC4Replicator2TLSScheme, "www.example.com"_sl, 4984, "/qat/_blipsync"_sl};
static const C4Address kOtherHostRequest{kC4Replicator2Scheme, "couchbase.com"_sl, 4984, "/beer/_blipsync"_sl};

TEST_CASE("CookieStore", "[Cookies]") {
    Retained<CookieStore> store = new CookieStore;
    CHECK(store->cookies().empty());
    CHECK(!store->changed());
    CHECK(store->cookiesForRequest(kRequest).empty());

    CHECK(store->setCookie("x=y; Domain=Example.Com", "example.com", "/"));
    CHECK(!store->cookies().empty());
    CHECK(!store->changed());  // it's non-persistent
    CHECK(store->setCookie("e=mc^2; Domain=WWW.Example.Com; Max-Age=30", "www.example.com", "/"));
    CHECK(store->setCookie("f=ma; Domain=www.ox.ac.uk; Expires=Tue, 09 Jun 2099 10:18:14 GMT", "www.ox.ac.uk", "/"));
    CHECK(store->changed());
    CHECK(store->setCookie("jens=awesome; Domain=snej.example.com", "example.com", "/"));
    CHECK(store->cookiesForRequest(kRequest) == "x=y; e=mc^2");
    CHECK(store->cookiesForRequest(kOtherPathRequest) == "x=y; e=mc^2");
    CHECK(store->cookiesForRequest(kSecureRequest) == "x=y; e=mc^2");
    CHECK(store->cookiesForRequest(kOtherHostRequest).empty());

    SECTION("Replace Cookie") {
        store->clearChanged();
        CHECK(store->setCookie("e=something else; Domain=WWW.Example.Com", "www.example.com", "/"));
        CHECK(store->changed());  // a persistent cookie got removed
        CHECK(store->cookiesForRequest(kRequest) == "x=y; e=something else");
    }
    SECTION("No-Op Replace Cookie") {
        store->clearChanged();
        CHECK(store->setCookie("x=y; Domain=Example.Com", "example.com", "/"));
        CHECK(store->setCookie("f=ma; Domain=www.ox.ac.uk; Expires=Tue, 09 Jun 2099 10:18:14 GMT", "www.ox.ac.uk",
                               "/"));
        CHECK(!store->changed());
    }
    SECTION("Secure Cookie") {
        CHECK(store->setCookie("password=123456; Domain=WWW.Example.Com; Secure=true", "www.example.com", "/"));
        CHECK(store->cookiesForRequest(kRequest) == "x=y; e=mc^2");
        CHECK(store->cookiesForRequest(kSecureRequest) == "x=y; e=mc^2; password=123456");
    }
    SECTION("Paths") {
        CHECK(store->setCookie("path=qat; Domain=example.com; Path=/qat", "example.com", "/"));
        CHECK(store->setCookie("path=Qat; Domain=example.com; Path=/Qat", "example.com", "/"));
        CHECK(store->setCookie("path=qaternion; Domain=example.com; Path=/qaternion", "example.com", "/"));
        CHECK(store->setCookie("x=z; Domain=Example.com; Path=/elsewhere", "example.com", "/"));
        CHECK(store->cookiesForRequest(kRequest) == "x=y; e=mc^2");
        CHECK(store->cookiesForRequest(kOtherPathRequest) == "x=y; e=mc^2; path=qat");
    }
    SECTION("Persistence") {
        alloc_slice encoded = store->encode();
        CHECK(encoded);
        Retained<CookieStore> store2 = new CookieStore(encoded);
        CHECK(store2->cookies().size() == 2);
        CHECK(!store2->changed());
        CHECK(store2->cookiesForRequest(kRequest) == "e=mc^2");
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "DatabaseCookies", "[Cookies]") {
    {
        // Set cookies:
        DatabaseCookies cookies(db);
        CHECK(cookies.cookiesForRequest(kRequest).empty());
        CHECK(cookies.setCookie("e=mc^2; Domain=WWW.Example.Com; Max-Age=30", string(slice(kRequest.hostname)),
                                string(slice(kRequest.path))));
        CHECK(cookies.setCookie("name=value", string(slice(kRequest.hostname)), string(slice(kRequest.path))));
        cookies.saveChanges();
    }
    {
        // Get the cookies, in the same C4Database instance:
        DatabaseCookies cookies(db);
        CHECK(cookies.cookiesForRequest(kRequest) == "e=mc^2; name=value");
    }
    {
        // Get the cookies, in a different C4Database instance while the 1st one is open:
        c4::ref<C4Database> db2 = c4db_openAgain(db, nullptr);
        DatabaseCookies     cookies(db2);
        CHECK(cookies.cookiesForRequest(kRequest) == "e=mc^2; name=value");
    }
    // Closing db causes the shared context to go away because there are no remaining handles
    reopenDB();
    {
        // Get the cookies, in a new C4Database instance -- only the persistent one survives:
        DatabaseCookies cookies(db);
        CHECK(cookies.cookiesForRequest(kRequest) == "e=mc^2");
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "c4 Cookie API", "[Cookies]") {
    const C4Address request = {slice(kRequest.scheme), slice(kRequest.hostname), kRequest.port, slice(kRequest.path)};
    C4Error         error;

    {
        // Set cookies:
        alloc_slice cookies = c4db_getCookies(db, request, &error);
        CHECK(!cookies);
        CHECK(error.code == 0);
        CHECK(c4db_setCookie(db, "e=mc^2; Domain=WWW.Example.Com; Max-Age=30"_sl, request.hostname, request.path, false,
                             WITH_ERROR(&error)));
        CHECK(c4db_setCookie(db, "dest=Example; Domain=Example.Com; Max-Age=30"_sl, request.hostname, request.path,
                             true, WITH_ERROR(&error)));
        CHECK(c4db_setCookie(db, "dest=entireWorld; Domain=.Com; Max-Age=30"_sl, request.hostname, request.path, true,
                             WITH_ERROR(&error)));
        {
            ExpectingExceptions x;
            CHECK_FALSE(c4db_setCookie(db, "dest=Example; Domain=Example.Com; Max-Age=30"_sl, request.hostname,
                                       request.path, false, &error));
            CHECK(error.domain == LiteCoreDomain);
            CHECK(error.code == kC4ErrorInvalidParameter);
            CHECK_FALSE(c4db_setCookie(db, "dest=entireWorld; Domain=.Com; Max-Age=30"_sl, request.hostname,
                                       request.path, false, &error));
            CHECK(error.domain == LiteCoreDomain);
            CHECK(error.code == kC4ErrorInvalidParameter);
        }
        CHECK(c4db_setCookie(db, "name=value"_sl, request.hostname, request.path, false, WITH_ERROR(&error)));
        CHECK(c4db_setCookie(db, "foo=bar; Path=/db"_sl, request.hostname, request.path, false, WITH_ERROR(&error)));
        CHECK(c4db_setCookie(db, "frob=baz; Path=/db/"_sl, request.hostname, request.path, false, WITH_ERROR(&error)));
        CHECK(c4db_setCookie(db, "eenie=meenie; Path=/db/xox"_sl, request.hostname, request.path, false,
                             WITH_ERROR(&error)));
        CHECK(c4db_setCookie(db, "minie=moe; Path=/someotherdb"_sl, request.hostname, request.path, false,
                             WITH_ERROR(&error)));
    }
    {
        // Get the cookies, in the same C4Database instance:
        alloc_slice cookies = c4db_getCookies(db, request, ERROR_INFO(error));
        CHECK(cookies == "e=mc^2; dest=Example; dest=entireWorld; name=value; foo=bar; frob=baz"_sl);
    }
    {
        // Get the cookies, in a different C4Database instance while the 1st one is open:
        c4::ref<C4Database> db2     = c4db_openAgain(db, nullptr);
        alloc_slice         cookies = c4db_getCookies(db2, request, ERROR_INFO(error));
        CHECK(cookies == "e=mc^2; dest=Example; dest=entireWorld; name=value; foo=bar; frob=baz"_sl);
    }

    reopenDB();

    {
        // Make sure the cookies are reloaded from storage:
        alloc_slice cookies = c4db_getCookies(db, request, ERROR_INFO(error));
        CHECK(cookies == "e=mc^2; dest=Example; dest=entireWorld"_sl);

        // Clear the cookies:
        c4db_clearCookies(db);
        CHECK(c4db_getCookies(db, request, WITH_ERROR(&error)) == nullslice);
    }

    reopenDB();

    {
        // Make sure the clear was saved:
        CHECK(c4db_getCookies(db, request, WITH_ERROR(&error)) == nullslice);
    }
}

TEST_CASE("RootPathMatch", "[Cookies]") {
    static const C4Address kRootPathRequest{kC4Replicator2Scheme, "example.com"_sl, 4984, "/"_sl};
    static const C4Address kEmptyPathRequest{kC4Replicator2Scheme, "example.com"_sl, 4984, ""_sl};

    Retained<CookieStore> store = new CookieStore;
    CHECK(store->setCookie("a1=b1; Domain=example.com; Path=/", "example.com", "/"));
    CHECK(store->setCookie("a2=b2; Domain=example.com; Path=/", "example.com", ""));
    CHECK(store->setCookie("a3=b3; Domain=example.com", "example.com", "/"));
    CHECK(store->setCookie("a4=b4; Domain=example.com", "example.com", ""));
    CHECK(store->cookiesForRequest(kRootPathRequest) == "a1=b1; a2=b2; a3=b3; a4=b4");
    CHECK(store->cookiesForRequest(kEmptyPathRequest) == "a1=b1; a2=b2; a3=b3; a4=b4");
}
