//
// ActorTest.cc
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


static Retained<AsyncProvider<string>> aProvider, bProvider;

static Async<string> provideA() {
    return aProvider;
}

static Async<string> provideB() {
    return bProvider;
}

static Async<string> provideSum() {
    string a, b;
    BEGIN_ASYNC_RETURNING(string)
    asyncCall(a, provideA());
    asyncCall(b, provideB());
    return a + b;
    END_ASYNC()
}


static Async<string> provideSumPlus() {
    string a;
    BEGIN_ASYNC_RETURNING(string)
    asyncCall(a, provideSum());
    return a + "!";
    END_ASYNC()
}


static Async<string> provideImmediately() {
    BEGIN_ASYNC_RETURNING(string)
    return "immediately";
    END_ASYNC()
}


static Async<int> provideLoop() {
    string n;
    int sum = 0;
    int i = 0;
    BEGIN_ASYNC_RETURNING(int)
    for (i = 0; i < 10; i++) {
        asyncCall(n, provideSum());
        //fprintf(stderr, "n=%f, i=%d, sum=%f\n", n, i, sum);
        sum += n.size() * i;
    }
    return sum;
    END_ASYNC()
}


static string provideNothingResult;

static void provideNothing() {
    string a, b;
    BEGIN_ASYNC()
    asyncCall(a, provideA());
    asyncCall(b, provideB());
    provideNothingResult = a + b;
    END_ASYNC()
}



TEST_CASE("Async", "[Async]") {
    aProvider = Async<string>::provider();
    bProvider = Async<string>::provider();
    {
        Async<string> sum = provideSum();
        REQUIRE(!sum.ready());
        aProvider->setResult("hi");
        REQUIRE(!sum.ready());
        bProvider->setResult(" there");
        REQUIRE(sum.ready());
        REQUIRE(sum.result() == "hi there");
    }
    aProvider = bProvider = nullptr;
    CHECK(AsyncContext::gInstanceCount == 0);
}


TEST_CASE("Async, other order", "[Async]") {
    aProvider = Async<string>::provider();
    bProvider = Async<string>::provider();
    {
        Async<string> sum = provideSum();
        REQUIRE(!sum.ready());
        bProvider->setResult(" there");    // this time provideB() finishes first
        REQUIRE(!sum.ready());
        aProvider->setResult("hi");
        REQUIRE(sum.ready());
        REQUIRE(sum.result() == "hi there");
    }
    aProvider = bProvider = nullptr;
    CHECK(AsyncContext::gInstanceCount == 0);
}


TEST_CASE("AsyncWaiter", "[Async]") {
    aProvider = Async<string>::provider();
    bProvider = Async<string>::provider();
    {
        Async<string> sum = provideSum();
        string result;
        sum.then([&](string s) {
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
    aProvider = bProvider = nullptr;
    CHECK(AsyncContext::gInstanceCount == 0);
}


TEST_CASE("Async, 2 levels", "[Async]") {
    aProvider = Async<string>::provider();
    bProvider = Async<string>::provider();
    {
        Async<string> sum = provideSumPlus();
        REQUIRE(!sum.ready());
        aProvider->setResult("hi");
        REQUIRE(!sum.ready());
        bProvider->setResult(" there");
        REQUIRE(sum.ready());
        REQUIRE(sum.result() == "hi there!");
    }
    aProvider = bProvider = nullptr;
    CHECK(AsyncContext::gInstanceCount == 0);
}


TEST_CASE("Async, loop", "[Async]") {
    aProvider = Async<string>::provider();
    bProvider = Async<string>::provider();
    {
        Async<int> sum = provideLoop();
        for (int i = 1; i <= 10; i++) {
            REQUIRE(!sum.ready());
            aProvider->setResult("hi");
            REQUIRE(!sum.ready());
            aProvider = Async<string>::provider();
            bProvider->setResult(" there");
            bProvider = Async<string>::provider();
        }
        REQUIRE(sum.ready());
        REQUIRE(sum.result() == 360);
    }
    aProvider = bProvider = nullptr;
    CHECK(AsyncContext::gInstanceCount == 0);
}


TEST_CASE("Async, immediately", "[Async]") {
    {
        Async<string> im = provideImmediately();
        REQUIRE(im.ready());
        REQUIRE(im.result() == "immediately");
    }
    CHECK(AsyncContext::gInstanceCount == 0);
}


TEST_CASE("Async void fn", "[Async]") {
    aProvider = Async<string>::provider();
    bProvider = Async<string>::provider();
    provideNothingResult = "";
    {
        provideNothing();
        REQUIRE(provideNothingResult == "");
        aProvider->setResult("hi");
        REQUIRE(provideNothingResult == "");
        bProvider->setResult(" there");
        REQUIRE(provideNothingResult == "hi there");
    }
    aProvider = bProvider = nullptr;
    CHECK(AsyncContext::gInstanceCount == 0);
}


#pragma mark - WITH ACTORS:


static Async<string> downloader(string url) {
    auto provider = Async<string>::provider();
    std::thread t([=] {
        std::this_thread::sleep_for(1s);
        provider->setResult("Contents of " + url);
    });
    t.detach();
    return provider;
}


static string waitFor(Async<string> &async) {
    optional<string> contents;
    async.then([&](string c) {
        contents = c;
    });
    while (!contents) {
        this_thread::sleep_for(10ms);
    }
    return *contents;
}


class TestActor : public Actor {
public:
    TestActor() :Actor(kC4Cpp_DefaultLog) { }
    
    Async<string> download(string url) {
        string contents;
        BEGIN_ASYNC_RETURNING(string)
        CHECK(currentActor() == this);
        asyncCall(contents, downloader(url));
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
        asyncCall(contents, *dl1);
        CHECK(currentActor() == this);
        asyncCall(string contents2, *dl2);
        return contents + " and " + contents2;
        END_ASYNC()
    }
};


TEST_CASE("Async on thread", "[Async]") {
    auto asyncContents = downloader("couchbase.com");
    string contents = waitFor(asyncContents);
    CHECK(contents == "Contents of couchbase.com");
}


TEST_CASE("Async Actor", "[Async]") {
    auto actor = make_retained<TestActor>();
    auto asyncContents = actor->download("couchbase.org");
    string contents = waitFor(asyncContents);
    CHECK(contents == "Contents of couchbase.org");
}


TEST_CASE("Async Actor Twice", "[Async]") {
    auto actor = make_retained<TestActor>();
    auto asyncContents = actor->download("couchbase.org", "couchbase.biz");
    string contents = waitFor(asyncContents);
    CHECK(contents == "Contents of couchbase.org and Contents of couchbase.biz");
}
