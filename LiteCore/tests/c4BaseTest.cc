//
// c4BaseTest.cc
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

#include "c4Internal.hh"
#include "c4Test.hh"
#include "InstanceCounted.hh"
#include "catch.hpp"
#include "NumConversion.hh"
#include "Actor.hh"
#include "URLTransformer.hh"
#include <exception>
#include <chrono>
#include <thread>
#ifdef WIN32
#include <winerror.h>
#endif

using namespace fleece;
using namespace std;
using namespace std::chrono_literals;
using namespace litecore::repl;


// NOTE: These tests have to be in the C++ tests target, not the C tests, because they use internal
// LiteCore symbols that aren't exported by the dynamic library.


#pragma mark - ERROR HANDLING:


TEST_CASE("C4Error messages") {
    C4Error errors[200];
    for (int i = 0; i < 200; i++) {
        char message[100];
        sprintf(message, "Error number %d", 1000+i);
        c4error_return(LiteCoreDomain, 1000+i, slice(message), &errors[i]);
    }
    for (int i = 0; i < 200; i++) {
        CHECK(errors[i].domain == LiteCoreDomain);
        CHECK(errors[i].code == 1000+i);
        alloc_slice message = c4error_getMessage(errors[i]);
        string messageStr = string(message);
        if (i >= (200 - kMaxErrorMessagesToSave)) {
            // The latest C4Errors generated will have their custom messages:
            char expected[100];
            sprintf(expected, "Error number %d", 1000+i);
            CHECK(messageStr == string(expected));
        } else {
            // The earlier C4Errors will have default messages for their code:
            CHECK(messageStr == "(unknown LiteCoreError)");
        }
    }

#ifdef WIN32
    const long errs[] = { WSAEADDRINUSE, WSAEADDRNOTAVAIL, WSAEAFNOSUPPORT, WSAEALREADY,
                          WSAECANCELLED, WSAECONNABORTED, WSAECONNREFUSED, WSAECONNRESET,
                          WSAEDESTADDRREQ, WSAEHOSTUNREACH, WSAEINPROGRESS, WSAEISCONN,
                          WSAELOOP, WSAEMSGSIZE, WSAENETDOWN, WSAENETRESET,
                          WSAENETUNREACH, WSAENOBUFS, WSAENOPROTOOPT, WSAENOTCONN,
                          WSAENOTSOCK, WSAEOPNOTSUPP, WSAEPROTONOSUPPORT, WSAEPROTOTYPE,
                          WSAETIMEDOUT, WSAEWOULDBLOCK };
    for(const auto err: errs) {
        error errObj(error::Domain::POSIX, int(err));
        string msg = errObj.what();
        CHECK(msg.find("Unknown error") == -1); // Should have a valid error message
        CHECK(errObj.code != err); // Should be remapped to standard POSIX code
    }
#endif
}

TEST_CASE("C4Error exceptions") {
    ++gC4ExpectExceptions;
    C4Error error;
    try {
        throw litecore::error(litecore::error::LiteCore,
                              litecore::error::InvalidParameter,
                              "Oops");
        FAIL("Exception wasn't thrown");
    } catchError(&error);
    --gC4ExpectExceptions;
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorInvalidParameter);
    alloc_slice message = c4error_getMessage(error);
    string messageStr = string(message);
    CHECK(messageStr == "Oops");
}


static string fakeErrorTest(int n, C4Error *outError) {
    if (n >= 0)
        return "ok";
    c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Dude, that's negative"_sl, outError);
    return "bad";
}


TEST_CASE("Error Backtraces", "[Errors][C]") {
    bool oldCapture = c4error_getCaptureBacktraces();

    c4error_setCaptureBacktraces(true);
    C4Error error = c4error_make(LiteCoreDomain, kC4ErrorUnimplemented, nullslice);
    alloc_slice backtrace = c4error_getBacktrace(error);
    C4Log("Got backtrace: %.*s", FMTSLICE(backtrace));
    CHECK(backtrace);

    c4error_setCaptureBacktraces(false);
    error = c4error_make(LiteCoreDomain, kC4ErrorUnimplemented, nullslice);
    backtrace = c4error_getBacktrace(error);
    CHECK(!backtrace);

    c4error_setCaptureBacktraces(oldCapture);
}


TEST_CASE("C4Error Reporting Macros", "[Errors][C]") {
    C4Error error;
    string result = fakeErrorTest(7, ERROR_INFO(error));
    CHECK(result == "ok");
    result = fakeErrorTest(-1, ERROR_INFO(error));

#if 0 // enable these to test actual test failures and warnings:
    CHECK(result == "ok");
    WARN(error);

    CHECK(fakeErrorTest(23, WITH_ERROR()) == "ok");
    CHECK(fakeErrorTest(-1, WITH_ERROR()) == "ok");
#endif

#if 0
    C4DatabaseConfig2 config = {"/ddddd"_sl, kC4DB_ReadOnly};
    CHECK(c4db_openNamed("xxxxx"_sl, &config, WITH_ERROR()));
#endif
}


#pragma mark - INSTANCECOUNTED:


namespace {

    class NonVirt {
    public:
        int64_t o_hai;
    };

    class Virt {
    public:
        int64_t foo;
        virtual ~Virt() { }
    };

    class NonVirtCounty : public NonVirt, public fleece::InstanceCountedIn<NonVirtCounty> {
    public:
        NonVirtCounty(int32_t b) :bar(b) { }
        int32_t bar;
    };

    class VirtCounty : public Virt, public fleece::InstanceCountedIn<VirtCounty> {
    public:
        VirtCounty(int32_t b) :bar(b) { }
        int32_t bar;
    };

    class TestActor : public litecore::actor::Actor {
    public:
        TestActor() 
            :Actor(kC4Cpp_DefaultLog, "TestActor")
        {}

        void doot() {
            enqueue(FUNCTION_TO_QUEUE(TestActor::_doot));    
        }

        void delayed_doot() {
            C4Log("I'LL DO IT LATER...");
            enqueueAfter(0.5s, FUNCTION_TO_QUEUE(TestActor::_doot));
        }

        void recursive_doot() {
            enqueue(FUNCTION_TO_QUEUE(TestActor::_recursive_doot));
        }

        void bad_doot() {
            enqueue(FUNCTION_TO_QUEUE(TestActor::_bad_doot));
        }

        void bad_recursive_doot() {
            enqueue(FUNCTION_TO_QUEUE(TestActor::_bad_recursive_doot));
        }

    private:
        void _doot() {
            C4Log("DOOT!");
        }

        void _recursive_doot() {
            C4Log("GETTING READY...");
            doot();
        }

        void _bad_doot() {
            throw std::runtime_error("TURN TO THE DARK SIDE");
        }

        void _bad_recursive_doot() {
            C4Log("LET THE HATE FLOW THROUGH YOU...");
            bad_doot();
        }
    };

    TEST_CASE("fleece::InstanceCounted") {
        auto baseInstances = InstanceCounted::count();
        auto n = new NonVirtCounty(12);
        auto v = new VirtCounty(34);
        C4Log("NonVirtCounty instance at %p; IC at %p", n, (fleece::InstanceCounted*)n);
        C4Log("VirtCounty instance at %p; IC at %p", v, (fleece::InstanceCountedIn<Virt>*)v);
        REQUIRE(InstanceCounted::count() == baseInstances + 2);
        c4_dumpInstances();
        delete n;
        delete v;
        REQUIRE(InstanceCounted::count() == baseInstances);
    }

    TEST_CASE("Narrow Cast") {
        CHECK(narrow_cast<long, uint64_t>(4) == 4);
        CHECK(narrow_cast<uint8_t, uint16_t>(128U) == 128U);
        CHECK(narrow_cast<uint8_t, int16_t>(128) == 128U);
        CHECK(narrow_cast<int8_t, int16_t>(64) == 64);
        CHECK(narrow_cast<int8_t, int16_t>(-1) == -1);

#if DEBUG
        {
            ExpectingExceptions x;
            CHECK_THROWS(narrow_cast<uint8_t, uint16_t>(UINT8_MAX + 1));
            CHECK_THROWS(narrow_cast<uint8_t, int16_t>(-1));
            CHECK_THROWS(narrow_cast<int8_t, int16_t>(INT16_MAX - 1));
        }
#else
        CHECK(narrow_cast<uint8_t, uint16_t>(UINT8_MAX + 1) == static_cast<uint8_t>(UINT8_MAX + 1));
        CHECK(narrow_cast<uint8_t, int8_t>(-1) == static_cast<uint8_t>(-1));
        CHECK(narrow_cast<int8_t, int16_t>(INT16_MAX - 1) == static_cast<int8_t>(INT16_MAX - 1));
#endif
    }

    TEST_CASE("Channel Manifest") {
        thread t[4];
        auto actor = retained(new TestActor());
        for(int i = 0; i < 4; i++) {
            t[i] = thread([&actor]() {
                actor->doot();
            });
        }

        actor->delayed_doot();
        t[0].join();
        t[1].join();
        t[2].join();
        t[3].join();

        actor->recursive_doot();
        this_thread::sleep_for(1s);
        
        ExpectingExceptions x;
        actor->bad_recursive_doot();
        this_thread::sleep_for(2s);
    }

    TEST_CASE("URL Transformation") {
        slice withPort, unaffected;
        alloc_slice withoutPort;
        SECTION("Plain") {
            withPort = "ws://duckduckgo.com:80/search"_sl;
            withoutPort = "ws://duckduckgo.com/search"_sl;
            unaffected = "ws://duckduckgo.com:4984/search"_sl;
        }

        SECTION("TLS") {
            withPort = "wss://duckduckgo.com:443/search"_sl;
            withoutPort = "wss://duckduckgo.com/search"_sl;
            unaffected = "wss://duckduckgo.com:4984/search"_sl;
        }

        alloc_slice asIsWithPort = transform_url(withPort, URLTransformStrategy::AsIs);
        alloc_slice asIsWithoutPort = transform_url(withoutPort, URLTransformStrategy::AsIs);
        alloc_slice asInUnaffected = transform_url(unaffected, URLTransformStrategy::AsIs);

        CHECK(asIsWithPort == withPort);
        CHECK(asIsWithoutPort == withoutPort);
        CHECK(asIsWithoutPort.buf == withoutPort.buf);
        CHECK(asInUnaffected == unaffected);

        alloc_slice addPortWithPort = transform_url(withPort, URLTransformStrategy::AddPort);
        alloc_slice addPortWithoutPort = transform_url(withoutPort, URLTransformStrategy::AddPort);
        alloc_slice addPortUnaffected = transform_url(unaffected, URLTransformStrategy::AddPort);

        CHECK(addPortWithPort == withPort);
        CHECK(addPortWithoutPort == withPort);
        CHECK(!addPortUnaffected);

        alloc_slice removePortWithPort = transform_url(withPort, URLTransformStrategy::RemovePort);
        alloc_slice removePortWithoutPort = transform_url(withoutPort, URLTransformStrategy::RemovePort);
        alloc_slice removePortUnaffected = transform_url(unaffected, URLTransformStrategy::RemovePort);

        CHECK(removePortWithPort == withoutPort);
        CHECK(removePortWithoutPort == withoutPort);
        CHECK(!removePortUnaffected);

        URLTransformStrategy strategy = URLTransformStrategy::AsIs;
        CHECK(++strategy == URLTransformStrategy::AddPort);
        CHECK(++strategy == URLTransformStrategy::RemovePort);
    }
}
