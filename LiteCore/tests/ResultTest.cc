//
// ResultTest.cc
//
// Copyright © 2022 Couchbase. All rights reserved.
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

#include "Result.hh"
#include "LiteCoreTest.hh"

using namespace std;
using namespace litecore;

static Result<string> rfunc(int x) {
    if ( x > 0 ) return to_string(x);
    else if ( x < 0 )
        return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
    else
        throw logic_error("I didn't expect a kind of Spanish Inquisition!");
}

static Result<void> rvfunc(int x) {
    if ( x > 0 ) return {};
    else if ( x < 0 )
        return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
    else
        throw logic_error("I didn't expect a kind of Spanish Inquisition!");
}

static string xfunc(int x) {
    if ( x >= 0 ) return to_string(x);
    else
        C4Error{LiteCoreDomain, kC4ErrorInvalidParameter}.raise();
}

TEST_CASE("Result", "[Result]") {
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
TEST_CASE("Result then", "[Result]") {
    SECTION("Success") {
        Result<size_t> r = rfunc(11).then([](string&& str) { return str.size(); });
        REQUIRE(r.ok());
        CHECK(r.value() == 2);
    }
    SECTION("Error") {
        Result<size_t> r = rfunc(-1).then([](string&& str) { return str.size(); });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success, returning Result") {
        Result<size_t> r = rfunc(11).then([](string&& str) -> Result<size_t> { return str.size(); });
        REQUIRE(r.ok());
        CHECK(r.value() == 2);
    }
    SECTION("Error, returning Result") {
        Result<size_t> r = rfunc(11).then([](string&& str) -> Result<size_t> {
            return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
        });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }
}

// Test Result<void>::then()
TEST_CASE("Result void then", "[Result]") {
    SECTION("Success") {
        Result<int> r = rvfunc(11).then([]() { return 2; });
        REQUIRE(r.ok());
        CHECK(r.value() == 2);
    }
    SECTION("Error") {
        Result<int> r = rvfunc(-1).then([]() { return 1; });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success, returning Result") {
        Result<int> r = rvfunc(11).then([]() -> Result<int> { return 2; });
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
TEST_CASE("Result then void", "[Result]") {
    SECTION("Success") {
        optional<string> calledWith;
        Result<void>     r = rfunc(11).then([&](string&& str) { calledWith = str; });
        REQUIRE(r.ok());
        CHECK(calledWith == "11");
    }
    SECTION("Error") {
        optional<string> calledWith;
        Result<void>     r = rfunc(-1).then([&](string&& str) { calledWith = str; });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success, returning Result") {
        optional<string> calledWith;
        Result<void>     r = rfunc(11).then([&](string&& str) -> Result<void> {
            calledWith = str;
            return {};
        });
        REQUIRE(r.ok());
        CHECK(calledWith == "11");
    }
    SECTION("Error, returning Result") {
        optional<string> calledWith;
        Result<void>     r = rfunc(11).then([&](string&& str) -> Result<void> {
            calledWith = str;
            return C4Error{LiteCoreDomain, kC4ErrorInvalidParameter};
        });
        REQUIRE(r.isError());
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
        CHECK(calledWith == "11");
    }
}

TEST_CASE("Result onError", "[Result]") {
    SECTION("Success") {
        optional<C4Error> calledWithErr;
        Result<string>    r = rfunc(11).onError([&](C4Error err) { calledWithErr = err; });
        REQUIRE(r.ok());
        CHECK(r.value() == "11");
        CHECK(!calledWithErr);
    }
    SECTION("Error") {
        optional<C4Error> calledWithErr;
        Result<string>    r = rfunc(-1).onError([&](C4Error err) { calledWithErr = err; });
        REQUIRE(r.isError());
        CHECK(calledWithErr == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }
}

TEST_CASE("CatchResult", "[Result]") {
    SECTION("Success") {
        auto r = CatchResult([] { return xfunc(4); });
        CHECK(r.value() == "4");
    }

    SECTION("Exception") {
        ExpectingExceptions x;
        auto                r = CatchResult([] { return xfunc(-1); });
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Success when lambda returns Result") {
        auto r = CatchResult([] { return rfunc(4); });
        CHECK(r.value() == "4");
    }

    SECTION("Error when lambda returns Result") {
        auto r = CatchResult([] { return rfunc(-1); });
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorInvalidParameter});
    }

    SECTION("Exception when lambda returns Result") {
        ExpectingExceptions x;
        auto                r = CatchResult([] { return rfunc(0); });
        CHECK(r.error() == C4Error{LiteCoreDomain, kC4ErrorAssertionFailed});
    }
}

TEST_CASE("TRY", "[Result]") {
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
