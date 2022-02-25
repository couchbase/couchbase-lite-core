//
// AsyncTest.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Async.hh"
#include "Actor.hh"
#include "LiteCoreTest.hh"
#include "Logging.hh"

using namespace std;
using namespace litecore::actor;


static Async<string> downloader(string url) {
    auto provider = Async<string>::makeProvider();
    std::thread t([=] {
        std::this_thread::sleep_for(1s);
        provider->setResult("Contents of " + url);
    });
    t.detach();
    return provider;
}


class AsyncTest {
public:
    Retained<AsyncProvider<string>> _aProvider;
    Retained<AsyncProvider<string>> _bProvider;

    AsyncProvider<string>* aProvider() {
        if (!_aProvider)
            _aProvider = Async<string>::makeProvider();
        return _aProvider;
    }

    AsyncProvider<string>* bProvider() {
        if (!_bProvider)
            _bProvider = Async<string>::makeProvider();
        return _bProvider;
    }

    Async<string> provideA() {
        return aProvider();
    }

    Async<string> provideB() {
        return bProvider();
    }

    Async<string> provideOne() {
        Log("provideSum: awaiting A");
        return provideA().then([=](string a) {
            Log("provideSum: awaiting B");
            return a;
        });
    }


    Async<string> provideSum() {
        Log("provideSum: awaiting A");
        return provideA().then([=](string a) {
            Log("provideSum: awaiting B");
            return provideB().then([=](string b) {
                Log("provideSum: returning");
                return a + b;
            });
        });
    }


    Async<string> provideSumPlus() {
        return provideSum().then([=](string a) {
            return a + "!";
        });
    }


    Async<string> provideImmediately() {
        return string("immediately");
    }


    Async<string> provideError() {
        return provideA().then([](string a) -> Async<string> {
            if (a.empty())
                return C4Error::make(LiteCoreDomain, kC4ErrorInvalidParameter, "Empty!");
            else
                return a;
        });
    }


    string provideNothingResult;

    void provideNothing() {
        provideA().then([=](string a) {
            Log("provideSum: awaiting B");
            provideB().then([=](string b) {
                provideNothingResult = a + b;
            });
        });
    }

};


TEST_CASE_METHOD(AsyncTest, "Async", "[Async]") {
    Async<string> sum = provideSum();
    REQUIRE(!sum.ready());
    _aProvider->setResult("hi");
    REQUIRE(!sum.ready());
    _bProvider->setResult(" there");
    REQUIRE(sum.ready());
    REQUIRE(sum.result() == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async, other order", "[Async]") {
    Async<string> sum = provideSum();
    REQUIRE(!sum.ready());
    bProvider()->setResult(" there");    // this time provideB() finishes first
    REQUIRE(!sum.ready());
    aProvider()->setResult("hi");
    REQUIRE(sum.ready());
    REQUIRE(sum.result() == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async, emplaceResult") {
    auto p = Async<string>::makeProvider();
    auto v = p->asyncValue();
    REQUIRE(!v.ready());
    p->emplaceResult('*', 6);
    REQUIRE(v.ready());
    CHECK(v.result() == "******");
}


TEST_CASE_METHOD(AsyncTest, "AsyncWaiter", "[Async]") {
    Async<string> sum = provideSum();
    string result;
    move(sum).then([&](string &&s) {
        result = s;
    });
    REQUIRE(!sum.ready());
    REQUIRE(result == "");
    _aProvider->setResult("hi");
    REQUIRE(!sum.ready());
    REQUIRE(result == "");
    _bProvider->setResult(" there");
    REQUIRE(sum.ready());
    REQUIRE(result == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async, 2 levels", "[Async]") {
    Async<string> sum = provideSumPlus();
    REQUIRE(!sum.ready());
    _aProvider->setResult("hi");
    REQUIRE(!sum.ready());
    _bProvider->setResult(" there");
    REQUIRE(sum.ready());
    REQUIRE(sum.result() == "hi there!");
}


TEST_CASE_METHOD(AsyncTest, "Async, immediately", "[Async]") {
    Async<string> im = provideImmediately();
    REQUIRE(im.ready());
    REQUIRE(im.result() == "immediately");
}


TEST_CASE_METHOD(AsyncTest, "Async void fn", "[Async]") {
    provideNothing();
    REQUIRE(provideNothingResult == "");
    _aProvider->setResult("hi");
    REQUIRE(provideNothingResult == "");
    _bProvider->setResult(" there");
    REQUIRE(provideNothingResult == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async then returning void", "[Async]") {
    optional<string> result;
    provideSum().then([&](string &&s) {
        Log("--Inside then fn; s = \"%s\"", s.c_str());
        result = s;
    });

    Log("--Providing aProvider");
    _aProvider->setResult("hi");
    Log("--Providing bProvider");
    _bProvider->setResult(" there");
    CHECK(result == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async then returning T", "[Async]") {
    Async<size_t> size = provideSum().then([](string &&s) {
        Log("--Inside then fn; s = \"%s\", returning %zu", s.c_str(), s.size());
        return s.size();
    });

    Log("--Providing aProvider");
    _aProvider->setResult("hi");
    Log("--Providing bProvider");
    _bProvider->setResult(" there");
    CHECK(size.blockingResult() == 8);
}


TEST_CASE_METHOD(AsyncTest, "Async then returning async T", "[Async]") {
    Async<string> dl = provideSum().then([](string &&s) {
        Log("--Inside then fn; s = \"%s\", returning %zu", s.c_str(), s.size());
        return downloader(s);
    });

    Log("--Providing aProvider");
    _aProvider->setResult("hi");
    Log("--Providing bProvider");
    _bProvider->setResult(" there");
    CHECK(dl.blockingResult() == "Contents of hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async Error", "[Async]") {
    Async<string> r = provideError();
    REQUIRE(!r.ready());
    SECTION("no error") {
        _aProvider->setResult("hi");
        REQUIRE(r.ready());
        CHECK(!r.error());
        CHECK(r.c4Error() == C4Error{});
        CHECK(r.result() == "hi");
    }
    SECTION("error") {
        _aProvider->setResult("");
        REQUIRE(r.ready());
        auto e = r.error();
        REQUIRE(e);
        CHECK(e->domain == error::LiteCore);
        CHECK(e->code == error::InvalidParameter);
        C4Error c4e = r.c4Error();
        CHECK(c4e == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }
}


#pragma mark - WITH ACTORS:


class AsyncTestActor : public Actor {
public:
    AsyncTestActor() :Actor(kC4Cpp_DefaultLog) { }
    
    Async<string> download(string url) {
        return asCurrentActor([=] {
            CHECK(currentActor() == this);
            return downloader(url).then([=](string contents) -> string {
                // When `then` is used inside an Actor method, the lambda is called on its queue:
                CHECK(currentActor() == this);
                return contents;
            });
        });
    }

    Async<string> download(string url1, string url2) {
        return asCurrentActor([=] {
            CHECK(currentActor() == this);
            return download(url1).then([=](string contents1) {
                return download(url2).then([=](string contents2) {
                    CHECK(currentActor() == this);
                    return contents1 + " and " + contents2;
                });
            });
        });
    }

    void testThen(string url) {
        asCurrentActor([=] {
            downloader(url).then([=](string &&s) {
                assert(currentActor() == this);
                testThenResult = move(s);
                testThenReady = true;
            });
        });
    }

    atomic<bool> testThenReady = false;
    optional<string> testThenResult;
};


TEST_CASE("Async on thread", "[Async]") {
    auto asyncContents = downloader("couchbase.com");
    string contents = asyncContents.blockingResult();
    CHECK(contents == "Contents of couchbase.com");
}


TEST_CASE("Async Actor", "[Async]") {
    auto actor = make_retained<AsyncTestActor>();
    auto asyncContents = actor->download("couchbase.org");
    string contents = asyncContents.blockingResult();
    CHECK(contents == "Contents of couchbase.org");
}


TEST_CASE("Async Actor Twice", "[Async]") {
    auto actor = make_retained<AsyncTestActor>();
    auto asyncContents = actor->download("couchbase.org", "couchbase.biz");
    string contents = asyncContents.blockingResult();
    CHECK(contents == "Contents of couchbase.org and Contents of couchbase.biz");
}

TEST_CASE("Async Actor with then", "[Async]") {
    auto actor = make_retained<AsyncTestActor>();
    actor->testThen("couchbase.xxx");
    CHECK(!actor->testThenReady);
    while (!actor->testThenReady)
        this_thread::sleep_for(10ms);
    CHECK(actor->testThenResult == "Contents of couchbase.xxx");
}
