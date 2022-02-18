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

    Async<string> provideSum() {
        Log("provideSum: entry");
        string a, b;
        BEGIN_ASYNC_RETURNING(string)
        Log("provideSum: awaiting A");
        XAWAIT(a, provideA());
        Log("provideSum: awaiting B");
        XAWAIT(b, provideB());
        Log("provideSum: returning");
        return a + b;
        END_ASYNC()
    }


    Async<string> provideSumPlus() {
        string a;
        BEGIN_ASYNC_RETURNING(string)
        XAWAIT(a, provideSum());
        return a + "!";
        END_ASYNC()
    }


    Async<string> XXprovideSumPlus() {
        string a;
        return Async<string>(thisActor(), [=](AsyncFnState &_async_state_) mutable
                                                    -> std::optional<string> {
            switch (_async_state_.currentLine()) {
                default:
                    if (_async_state_.await(provideSum(), 78)) return {};
                case 78:
                    a = _async_state_.awaited<async_result_type<decltype(provideSum())>>()
                                        ->extractResult();
                    return a + "!";
            }
        });
    }


    Async<string> provideImmediately() {
        BEGIN_ASYNC_RETURNING(string)
        return "immediately";
        END_ASYNC()
    }


    Async<int> provideLoop() {
        string n;
        int sum = 0;
        int i = 0;
        BEGIN_ASYNC_RETURNING(int)
        for (i = 0; i < 10; i++) {
            XAWAIT(n, provideSum());
            //fprintf(stderr, "n=%f, i=%d, sum=%f\n", n, i, sum);
            sum += n.size() * i;
        }
        return sum;
        END_ASYNC()
    }


    string provideNothingResult;

    void provideNothing() {
        string a, b;
        BEGIN_ASYNC()
        XAWAIT(a, provideA());
        XAWAIT(b, provideB());
        provideNothingResult = a + b;
        END_ASYNC()
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
    sum.then([&](string &&s) {
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


TEST_CASE_METHOD(AsyncTest, "Async, loop", "[Async]") {
    Async<int> sum = provideLoop();
    for (int i = 1; i <= 10; i++) {
        REQUIRE(!sum.ready());
        _aProvider->setResult("hi");
        _aProvider = nullptr;
        REQUIRE(!sum.ready());
        _bProvider->setResult(" there");
        _bProvider = nullptr;
    }
    REQUIRE(sum.ready());
    REQUIRE(sum.result() == 360);
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


#pragma mark - WITH ACTORS:


class AsyncTestActor : public Actor {
public:
    AsyncTestActor() :Actor(kC4Cpp_DefaultLog) { }
    
    Async<string> download(string url) {
        string contents;
        BEGIN_ASYNC_RETURNING(string)
        CHECK(currentActor() == this);
        XAWAIT(contents, downloader(url));
        CHECK(currentActor() == this);
        return contents;
        END_ASYNC()
    }

    Async<string> download(string url1, string url2) {
        optional<Async<string>> dl1, dl2;
        string contents;
        BEGIN_ASYNC_RETURNING(string)
        CHECK(currentActor() == this);
        dl1 = download(url1);
        dl2 = download(url2);
        XAWAIT(contents, *dl1);
        CHECK(currentActor() == this);
        XAWAIT(string contents2, *dl2);
        return contents + " and " + contents2;
        END_ASYNC()
    }

    void testThen(string url) {
        BEGIN_ASYNC()
        downloader(url).then([=](string &&s) {
            // When `then` is used inside an Actor method, the lambda must be called on its queue:
            assert(currentActor() == this);
            testThenResult = move(s);
            testThenReady = true;
        });
        END_ASYNC()
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
