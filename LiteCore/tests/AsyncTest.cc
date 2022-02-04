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


template <typename T>
static T waitFor(Async<T> &async) {
    C4Log("Waiting...");
    optional<T> result;
    async.then([&](T &&c) {
        result = c;
    });
    while (!result) {
        this_thread::sleep_for(10ms);
    }
    C4Log("...done waiting");
    return *result;
}


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
    Retained<AsyncProvider<string>> aProvider = Async<string>::makeProvider();
    Retained<AsyncProvider<string>> bProvider = Async<string>::makeProvider();

    Async<string> provideA() {
        return aProvider;
    }

    Async<string> provideB() {
        return bProvider;
    }

    Async<string> provideSum() {
        Log("provideSum: entry");
        string a, b;
        BEGIN_ASYNC_RETURNING(string)
        Log("provideSum: awaiting A");
        AWAIT(a, provideA());
        Log("provideSum: awaiting B");
        AWAIT(b, provideB());
        Log("provideSum: returning");
        return a + b;
        END_ASYNC()
    }


    Async<string> provideSumPlus() {
        string a;
        BEGIN_ASYNC_RETURNING(string)
        AWAIT(a, provideSum());
        return a + "!";
        END_ASYNC()
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
            AWAIT(n, provideSum());
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
        AWAIT(a, provideA());
        AWAIT(b, provideB());
        provideNothingResult = a + b;
        END_ASYNC()
    }

};


TEST_CASE_METHOD(AsyncTest, "Async", "[Async]") {
    Async<string> sum = provideSum();
    REQUIRE(!sum.ready());
    aProvider->setResult("hi");
    REQUIRE(!sum.ready());
    bProvider->setResult(" there");
    REQUIRE(sum.ready());
    REQUIRE(sum.result() == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async, other order", "[Async]") {
    Async<string> sum = provideSum();
    REQUIRE(!sum.ready());
    bProvider->setResult(" there");    // this time provideB() finishes first
    REQUIRE(!sum.ready());
    aProvider->setResult("hi");
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
    aProvider->setResult("hi");
    REQUIRE(!sum.ready());
    REQUIRE(result == "");
    bProvider->setResult(" there");
    REQUIRE(sum.ready());
    REQUIRE(result == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async, 2 levels", "[Async]") {
    Async<string> sum = provideSumPlus();
    REQUIRE(!sum.ready());
    aProvider->setResult("hi");
    REQUIRE(!sum.ready());
    bProvider->setResult(" there");
    REQUIRE(sum.ready());
    REQUIRE(sum.result() == "hi there!");
}


TEST_CASE_METHOD(AsyncTest, "Async, loop", "[Async]") {
    Async<int> sum = provideLoop();
    for (int i = 1; i <= 10; i++) {
        REQUIRE(!sum.ready());
        aProvider->setResult("hi");
        REQUIRE(!sum.ready());
        aProvider = Async<string>::makeProvider();
        bProvider->setResult(" there");
        bProvider = Async<string>::makeProvider();
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
    aProvider->setResult("hi");
    REQUIRE(provideNothingResult == "");
    bProvider->setResult(" there");
    REQUIRE(provideNothingResult == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async then returning void", "[Async]") {
    optional<string> result;
    provideSum().then([&](string &&s) {
        Log("--Inside then fn; s = \"%s\"", s.c_str());
        result = s;
    });

    Log("--Providing aProvider");
    aProvider->setResult("hi");
    Log("--Providing bProvider");
    bProvider->setResult(" there");
    CHECK(result == "hi there");
}


TEST_CASE_METHOD(AsyncTest, "Async then returning T", "[Async]") {
    Async<size_t> size = provideSum().then([](string &&s) {
        Log("--Inside then fn; s = \"%s\", returning %zu", s.c_str(), s.size());
        return s.size();
    });

    Log("--Providing aProvider");
    aProvider->setResult("hi");
    Log("--Providing bProvider");
    bProvider->setResult(" there");
    CHECK(waitFor(size) == 8);
}


TEST_CASE_METHOD(AsyncTest, "Async then returning async T", "[Async]") {
    Async<string> dl = provideSum().then([](string &&s) {
        Log("--Inside then fn; s = \"%s\", returning %zu", s.c_str(), s.size());
        return downloader(s);
    });

    Log("--Providing aProvider");
    aProvider->setResult("hi");
    Log("--Providing bProvider");
    bProvider->setResult(" there");
    CHECK(waitFor(dl) == "Contents of hi there");
}


#pragma mark - WITH ACTORS:


class AsyncTestActor : public Actor {
public:
    AsyncTestActor() :Actor(kC4Cpp_DefaultLog) { }
    
    Async<string> download(string url) {
        string contents;
        BEGIN_ASYNC_RETURNING(string)
        CHECK(currentActor() == this);
        AWAIT(contents, downloader(url));
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
        AWAIT(contents, *dl1);
        CHECK(currentActor() == this);
        AWAIT(string contents2, *dl2);
        return contents + " and " + contents2;
        END_ASYNC()
    }

    void testThen(string url) {
        BEGIN_ASYNC()
        downloader(url).then([=](string &&s) {
            // When `then` is used inside an Actor method, the lambda must be called on its queue:
            CHECK(currentActor() == this);
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
    string contents = waitFor(asyncContents);
    CHECK(contents == "Contents of couchbase.com");
}


TEST_CASE("Async Actor", "[Async]") {
    auto actor = make_retained<AsyncTestActor>();
    auto asyncContents = actor->download("couchbase.org");
    string contents = waitFor(asyncContents);
    CHECK(contents == "Contents of couchbase.org");
}


TEST_CASE("Async Actor Twice", "[Async]") {
    auto actor = make_retained<AsyncTestActor>();
    auto asyncContents = actor->download("couchbase.org", "couchbase.biz");
    string contents = waitFor(asyncContents);
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
