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


#pragma mark - RESULT:


static Result<string> rfunc(int x) {
    if (x > 0)
        return to_string(x);
    else if (x < 0)
        return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
    else
        throw logic_error("I didn't expect a kind of Spanish Inquisition!");
}


static Result<void> rvfunc(int x) {
    if (x > 0)
        return {};
    else if (x < 0)
        return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
    else
        throw logic_error("I didn't expect a kind of Spanish Inquisition!");
}


static string xfunc(int x) {
    if (x >= 0)
        return to_string(x);
    else
        C4Error{LiteCoreDomain, kC4ErrorInvalidParameter}.raise();
}


TEST_CASE("Result", "[Async]") {
    auto r = rfunc(1);
    CHECK(r.ok());
    CHECK(r.value() == "1");
    CHECK(r.error() == kC4NoError);
    CHECK(r.errorPtr() == nullptr);

    r = rfunc(-1);
    CHECK(!r.ok());
    CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    CHECK(r.errorPtr() != nullptr);
    CHECK(*r.errorPtr() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
}


// Test Result<string>::then
TEST_CASE("Result then", "[Async]") {
    SECTION("Success") {
        Result<size_t> r = rfunc(11).then([](string &&str) { return str.size();});
        REQUIRE(r.ok());
        CHECK(r.value() == 2);
    }
    SECTION("Error") {
        Result<size_t> r = rfunc(-1).then([](string &&str) { return str.size();});
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success, returning Result") {
        Result<size_t> r = rfunc(11).then([](string &&str) -> Result<size_t> { return str.size();});
        REQUIRE(r.ok());
        CHECK(r.value() == 2);
    }
    SECTION("Error, returning Result") {
        Result<size_t> r = rfunc(11).then([](string &&str) -> Result<size_t> {
            return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
        });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }
}


// Test Result<void>::then()
TEST_CASE("Result void then", "[Async]") {
    SECTION("Success") {
        Result<int> r = rvfunc(11).then([]() { return 2;});
        REQUIRE(r.ok());
        CHECK(r.value() == 2);
    }
    SECTION("Error") {
        Result<int> r = rvfunc(-1).then([]() { return 1;});
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success, returning Result") {
        Result<int> r = rvfunc(11).then([]() -> Result<int> { return 2;});
        REQUIRE(r.ok());
        CHECK(r.value() == 2);
    }
    SECTION("Error, returning Result") {
        Result<int> r = rvfunc(11).then([]() -> Result<int> {
            return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
        });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }
}


// Test Result<string>.then(), where the fn returns void
TEST_CASE("Result then void", "[Async]") {
    SECTION("Success") {
        optional<string> calledWith;
        Result<void> r = rfunc(11).then([&](string &&str) { calledWith = str; });
        REQUIRE(r.ok());
        CHECK(calledWith == "11");
    }
    SECTION("Error") {
        optional<string> calledWith;
        Result<void> r = rfunc(-1).then([&](string &&str) { calledWith = str; });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success, returning Result") {
        optional<string> calledWith;
        Result<void> r = rfunc(11).then([&](string &&str) -> Result<void> {
            calledWith = str; return {};
        });
        REQUIRE(r.ok());
        CHECK(calledWith == "11");
    }
    SECTION("Error, returning Result") {
        optional<string> calledWith;
        Result<void> r = rfunc(11).then([&](string &&str) -> Result<void> {
            calledWith = str; return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
        });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
        CHECK(calledWith == "11");
    }
}


TEST_CASE("Result onError", "[Async]") {
    SECTION("Success") {
        optional<C4Error> calledWithErr;
        Result<string> r = rfunc(11).onError([&](C4Error err) {calledWithErr = err;});
        REQUIRE(r.ok());
        CHECK(r.value() == "11");
        CHECK(!calledWithErr);
    }
    SECTION("Error") {
        optional<C4Error> calledWithErr;
        Result<string> r = rfunc(-1).onError([&](C4Error err) {calledWithErr = err;});
        REQUIRE(r.isError());
        CHECK(calledWithErr == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }
}

TEST_CASE("TryResult", "[Async]") {
    SECTION("Success") {
        auto r = TryResult([]{ return xfunc(4);});
        CHECK(r.value() == "4");
    }

    SECTION("Exception") {
        ExpectingExceptions x;
        auto r = TryResult([]{ return xfunc(-1);});
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success when lambda returns Result") {
        auto r = TryResult([]{ return rfunc(4);});
        CHECK(r.value() == "4");
    }

    SECTION("Error when lambda returns Result") {
        auto r = TryResult([]{ return rfunc(-1);});
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Exception when lambda returns Result") {
        ExpectingExceptions x;
        auto r = TryResult([]{ return rfunc(0);});
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorAssertionFailed});
    }
}


TEST_CASE("TRY", "[Async]") {
    auto fn = [](int x) -> Result<size_t> {
        TRY(string str, rfunc(x));
        TRY(string str2, rfunc(x));
        return str.size();
    };

    Result<size_t> r = fn(1234);
    REQUIRE(r.ok());
    CHECK(r.value() == 4);

    r = fn(-1);
    REQUIRE(!r.ok());
    CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
}


#pragma mark - ASYNC:


static Async<string> downloader(string url) {
    auto provider = Async<string>::makeProvider();
    std::thread t([=] {
        std::this_thread::sleep_for(1s);
        provider->setResult("Contents of " + url);
    });
    t.detach();
    return provider->asyncValue();
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
        return aProvider()->asyncValue();
    }

    Async<string> provideB() {
        return bProvider()->asyncValue();
    }

    Async<string> provideDouble() {
        Log("provideSum: awaiting A");
        return provideA().then([=](string a) -> string {
            return a + a;
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

    Async<void> provideNothing() {
        return provideA().then([=](string a) {
            Log("provideNothing: awaiting B");
            return provideB().then([=](string b) -> Async<void> {
                Log("provideNothing: got B");
                provideNothingResult = a + b;
                return C4Error{};
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
    REQUIRE(sum.result().value() == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async, other order", "[Async]") {
    Async<string> sum = provideSum();
    REQUIRE(!sum.ready());
    bProvider()->setResult(" there");    // this time provideB() finishes first
    REQUIRE(!sum.ready());
    aProvider()->setResult("hi");
    REQUIRE(sum.ready());
    REQUIRE(sum.result().value() == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async, emplaceResult") {
    auto p = Async<string>::makeProvider();
    auto v = p->asyncValue();
    REQUIRE(!v.ready());
    p->setResult("******");
    REQUIRE(v.ready());
    CHECK(v.result().value() == "******");
}


TEST_CASE_METHOD(AsyncTest, "Async then", "[Async]") {
    Async<string> s = provideDouble();
    REQUIRE(!s.ready());
    _aProvider->setResult("Twice");
    REQUIRE(s.ready());
    CHECK(s.result().value() == "TwiceTwice");
}


TEST_CASE_METHOD(AsyncTest, "AsyncWaiter", "[Async]") {
    Async<string> sum = provideSum();
    string result;
    move(sum).then([&](string s) {
        result = s;
    }, assertNoError);
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
    REQUIRE(sum.result().value() == "hi there!");
}


TEST_CASE_METHOD(AsyncTest, "Async, immediately", "[Async]") {
    Async<string> im = provideImmediately();
    REQUIRE(im.ready());
    REQUIRE(im.result().value() == "immediately");
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
    }, assertNoError);

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
    CHECK(size.blockingResult().value() == 8);
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
    CHECK(dl.blockingResult().value() == "Contents of hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async Error", "[Async]") {
    Async<string> r = provideError();
    REQUIRE(!r.ready());
    SECTION("no error") {
        _aProvider->setResult("hi");
        REQUIRE(r.ready());
        CHECK(!r.error());
        CHECK(r.result().value() == "hi");
    }
    SECTION("error") {
        _aProvider->setResult("");
        REQUIRE(r.ready());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }
}


TEST_CASE_METHOD(AsyncTest, "Async Error Then", "[Async]") {
    optional<string> theStr;
    optional<C4Error> theError;
    provideError().then([&](string str) -> void {
        theStr = str;
    }).onError([&](C4Error err) {
        theError = err;
    });
    REQUIRE(!theStr);
    REQUIRE(!theError);

    SECTION("no error") {
        _aProvider->setResult("hi");
        CHECK(!theError);
        REQUIRE(theStr);
        CHECK(*theStr == "hi");
    }
    SECTION("error") {
        _aProvider->setResult("");
        CHECK(!theStr);
        REQUIRE(theError);
        CHECK(*theError == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
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
            }, assertNoError);
        });
    }

    atomic<bool> testThenReady = false;
    optional<string> testThenResult;
};


TEST_CASE("Async on thread", "[Async]") {
    auto asyncContents = downloader("couchbase.com");
    string contents = asyncContents.blockingResult().value();
    CHECK(contents == "Contents of couchbase.com");
}


TEST_CASE("Async Actor", "[Async]") {
    auto actor = make_retained<AsyncTestActor>();
    auto asyncContents = actor->download("couchbase.org");
    string contents = asyncContents.blockingResult().value();
    CHECK(contents == "Contents of couchbase.org");
}


TEST_CASE("Async Actor Twice", "[Async]") {
    auto actor = make_retained<AsyncTestActor>();
    auto asyncContents = actor->download("couchbase.org", "couchbase.biz");
    string contents = asyncContents.blockingResult().value();
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
