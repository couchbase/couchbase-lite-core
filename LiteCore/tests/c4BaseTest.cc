//
// c4BaseTest.cc
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
#include "c4Internal.hh"
#include "c4Collection.h"
#include "c4ExceptionUtils.hh"
#include "fleece/InstanceCounted.hh"
#include "catch.hpp"
#include "DatabaseImpl.hh"
#include "NumConversion.hh"
#include "Actor.hh"
#include "URLTransformer.hh"
#include "SQLiteDataFile.hh"
#include <exception>
#include <chrono>
#include <thread>
#ifdef WIN32
#    include "Error.hh"
#    include <winerror.h>
#endif
#include <future>
#include <sstream>

using namespace fleece;
using namespace std;
using namespace std::chrono_literals;
using namespace litecore::repl;

using error = litecore::error;

// NOTE: These tests have to be in the C++ tests target, not the C tests, because they use internal
// LiteCore symbols that aren't exported by the dynamic library.


#pragma mark - ERROR HANDLING:

TEST_CASE("C4Error messages") {
    C4Error          errors[200];
    constexpr size_t messageBufSize = 100, expectedBufSize = 100;
    for ( int i = 0; i < 200; i++ ) {
        char message[messageBufSize];
        snprintf(message, messageBufSize, "Error number %d", 1000 + i);
        c4error_return(LiteCoreDomain, 1000 + i, slice(message), &errors[i]);
    }
    for ( int i = 0; i < 200; i++ ) {
        CHECK(errors[i].domain == LiteCoreDomain);
        CHECK(errors[i].code == 1000 + i);
        alloc_slice message    = c4error_getMessage(errors[i]);
        string      messageStr = string(message);
        if ( i >= (200 - litecore::kMaxErrorMessagesToSave) ) {
            // The latest C4Errors generated will have their custom messages:
            char expected[expectedBufSize];
            snprintf(expected, expectedBufSize, "Error number %d", 1000 + i);
            CHECK(messageStr == string(expected));
        } else {
            // The earlier C4Errors will have default messages for their code:
            CHECK(messageStr == "(unknown LiteCoreError)");
        }
    }

#ifdef WIN32
    const long errs[] = {WSAEADDRINUSE,   WSAEADDRNOTAVAIL, WSAEAFNOSUPPORT,    WSAEALREADY,     WSAECANCELLED,
                         WSAECONNABORTED, WSAECONNREFUSED,  WSAECONNRESET,      WSAEDESTADDRREQ, WSAEHOSTUNREACH,
                         WSAEINPROGRESS,  WSAEISCONN,       WSAELOOP,           WSAEMSGSIZE,     WSAENETDOWN,
                         WSAENETRESET,    WSAENETUNREACH,   WSAENOBUFS,         WSAENOPROTOOPT,  WSAENOTCONN,
                         WSAENOTSOCK,     WSAEOPNOTSUPP,    WSAEPROTONOSUPPORT, WSAEPROTOTYPE,   WSAETIMEDOUT,
                         WSAEWOULDBLOCK};
    for ( const auto err : errs ) {
        litecore::error errObj(litecore::error::Domain::POSIX, int(err));
        string          msg = errObj.what();
        CHECK(msg.find("Unknown error") == -1);  // Should have a valid error message
        CHECK(errObj.code != err);               // Should be remapped to standard POSIX code
    }
#endif
}

TEST_CASE("C4Error exceptions") {
    ++gC4ExpectExceptions;
    C4Error err;
    try {
        throw litecore::error(litecore::error::LiteCore, litecore::error::InvalidParameter, "Oops");
        FAIL("Exception wasn't thrown");
    }
    catchError(&err);
    --gC4ExpectExceptions;
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorInvalidParameter);
    alloc_slice message    = c4error_getMessage(err);
    string      messageStr = string(message);
    CHECK(messageStr == "Oops");
}

static string fakeErrorTest(int n, C4Error* outError) {
    if ( n >= 0 ) return "ok";
    c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Dude, that's negative"_sl, outError);
    return "bad";
}

TEST_CASE("Error Backtraces", "[Errors][C]") {
    bool oldCapture = c4error_getCaptureBacktraces();

    c4error_setCaptureBacktraces(true);
    C4Error     error     = c4error_make(LiteCoreDomain, kC4ErrorUnimplemented, nullslice);
    alloc_slice backtrace = c4error_getBacktrace(error);
    C4Log("Got backtrace: %.*s", FMTSLICE(backtrace));
    CHECK(backtrace);

    c4error_setCaptureBacktraces(false);
    error     = c4error_make(LiteCoreDomain, kC4ErrorUnimplemented, nullslice);
    backtrace = c4error_getBacktrace(error);
    CHECK(!backtrace);

    c4error_setCaptureBacktraces(oldCapture);
}

TEST_CASE("C4Error Reporting Macros", "[Errors][C]") {
    C4Error error;
    string  result = fakeErrorTest(7, ERROR_INFO(error));
    CHECK(result == "ok");
    result = fakeErrorTest(-1, ERROR_INFO(error));

#if 0  // enable these to test actual test failures and warnings:
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

N_WAY_TEST_CASE_METHOD(C4Test, "Create collection concurrently", "[Database][C]") {
    const slice             dbName = db->getName();
    const C4DatabaseConfig2 config = db->getConfiguration();

    c4::ref db2 = c4db_openNamed(dbName, &config, ERROR_INFO());
    REQUIRE(db2);

    char buf[6]{};
    for ( int i = 0; i < 5; i++ ) {
        C4Error err{};
        C4Error err2{};

        snprintf(buf, 6, "coll%i", i);

        {
            slice                  collName{buf};
            const C4CollectionSpec spec{collName, "scope"_sl};

            auto a1 = std::async(std::launch::async, c4db_createCollection, db, spec, ERROR_INFO(&err));
            auto a2 = std::async(std::launch::async, c4db_createCollection, db2.get(), spec, ERROR_INFO(&err2));
        }

        CHECK(err.code == 0);
        CHECK(err2.code == 0);
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "Database Flag FullSync", "[Database][C]") {
    // Ensure that, by default, diskSyncFull is false.
    CHECK(!litecore::asInternal(db)->dataFile()->options().diskSyncFull);

    C4DatabaseConfig2 config = *c4db_getConfig2(db);
    config.flags |= kC4DB_DiskSyncFull;

    std::stringstream ss;
    ss << std::string(c4db_getName(db)) << "_" << c4_now();
    c4::ref<C4Database> dbWithFullSync = c4db_openNamed(slice(ss.str().c_str()), &config, ERROR_INFO());
    // The flag in config is passed to DataFile options.
    CHECK(litecore::asInternal(dbWithFullSync)->dataFile()->options().diskSyncFull);

    config.flags &= ~kC4DB_DiskSyncFull;
    c4::ref<C4Database> otherConnection = c4db_openNamed(c4db_getName(dbWithFullSync), &config, ERROR_INFO());
    // The flag applies per connection opened with the config.
    CHECK(!litecore::asInternal(otherConnection)->dataFile()->options().diskSyncFull);

    c4::ref<C4Database> againConnection = c4db_openAgain(dbWithFullSync, ERROR_INFO());
    // The flag is passed to database opened by openAgain.
    CHECK(litecore::asInternal(againConnection)->dataFile()->options().diskSyncFull);

    // https://www.sqlite.org/pragma.html#pragma_synchronous
    // 1 == "normal"
    // 2 == "full"

    alloc_slice defaultSyncPragma =
            litecore::asInternal(otherConnection)->dataFile()->rawScalarQuery("PRAGMA synchronous");
    CHECK(defaultSyncPragma == "1");

    alloc_slice fullSyncPragma = litecore::asInternal(dbWithFullSync)->dataFile()->rawScalarQuery("PRAGMA synchronous");
    CHECK(fullSyncPragma == "2");
}

// https://www.sqlite.org/mmap.html#:~:text=To%20disable%20memory%2Dmapped%20I,any%20content%20beyond%20N%20bytes.
N_WAY_TEST_CASE_METHOD(C4Test, "Database Flag MMap", "[Database][C]") {
    // Ensure that, by default, mmapDisabled is false.
    CHECK(!litecore::asInternal(db)->dataFile()->options().mmapDisabled);

    C4DatabaseConfig2 config = *c4db_getConfig2(db);
    config.flags |= kC4DB_MmapDisabled;

    std::stringstream ss;
    ss << std::string(c4db_getName(db)) << "_" << c4_now();

    c4::ref dbWithMmapDisabled = c4db_openNamed(slice(ss.str().c_str()), &config, ERROR_INFO());
    CHECK(litecore::asInternal(dbWithMmapDisabled)->dataFile()->options().mmapDisabled);

    config.flags ^= kC4DB_MmapDisabled;

    c4::ref dbWithDefaultConfig = c4db_openNamed(slice(ss.str().c_str()), &config, ERROR_INFO());
    // Another connection opened to the same database with `openNamed` and the default config will have mmap enabled.
    CHECK(!litecore::asInternal(dbWithDefaultConfig)->dataFile()->options().mmapDisabled);

    c4::ref dbAgain = c4db_openAgain(dbWithMmapDisabled, ERROR_INFO());
    // The flag is passed to the database opened by openAgain.
    CHECK(litecore::asInternal(dbAgain)->dataFile()->options().mmapDisabled);

#if TARGET_OS_OSX || TARGET_OS_SIMULATOR
    // Mmap is disabled on iOS Simulator and MacOS
    const auto defaultMmapStr = alloc_slice("0");
#else
    ss = std::stringstream{};
    ss << litecore::SQLiteDataFile::defaultMmapSize();
    const auto defaultMmapStr = alloc_slice(ss.str());
#endif

    alloc_slice defaultPragma =
            litecore::asInternal(dbWithDefaultConfig)->dataFile()->rawScalarQuery("PRAGMA mmap_size");
    CHECK(defaultPragma == defaultMmapStr);

    alloc_slice disabledPragma =
            litecore::asInternal(dbWithMmapDisabled)->dataFile()->rawScalarQuery("PRAGMA mmap_size");
    CHECK(disabledPragma == "0");
}

#pragma mark - INSTANCECOUNTED:

namespace {

    class NonVirt {
      public:
        int64_t o_hai;
    };

    class Virt {
      public:
        int64_t  foo{};
        virtual ~Virt() = default;
    };

    class NonVirtCounty
        : public NonVirt
        , public fleece::InstanceCountedIn<NonVirtCounty> {
      public:
        explicit NonVirtCounty(int32_t b) : NonVirt(), bar(b) {}

        int32_t bar;
    };

    class VirtCounty
        : public Virt
        , public fleece::InstanceCountedIn<VirtCounty> {
      public:
        explicit VirtCounty(int32_t b) : bar(b) {}

        int32_t bar;
    };

    class TestActor : public litecore::actor::Actor {
      public:
        TestActor() : Actor(litecore::kC4Cpp_DefaultLog, "TestActor") {}

        void doot() { enqueue(FUNCTION_TO_QUEUE(TestActor::_doot)); }

        void delayed_doot() {
            C4Log("I'LL DO IT LATER...");
            enqueueAfter(0.5s, FUNCTION_TO_QUEUE(TestActor::_doot));
        }

        void recursive_doot() { enqueue(FUNCTION_TO_QUEUE(TestActor::_recursive_doot)); }

        void bad_doot() { enqueue(FUNCTION_TO_QUEUE(TestActor::_bad_doot)); }

        void bad_recursive_doot() { enqueue(FUNCTION_TO_QUEUE(TestActor::_bad_recursive_doot)); }

      private:
        void _doot() { C4Log("DOOT!"); }

        void _recursive_doot() {
            C4Log("GETTING READY...");
            doot();
        }

        void _bad_doot() { throw std::runtime_error("TURN TO THE DARK SIDE"); }

        void _bad_recursive_doot() {
            C4Log("LET THE HATE FLOW THROUGH YOU...");
            bad_doot();
        }
    };

    TEST_CASE("fleece::InstanceCounted") {
        auto baseInstances = InstanceCounted::liveInstanceCount();
        auto n             = new NonVirtCounty(12);
        auto v             = new VirtCounty(34);
        C4Log("NonVirtCounty instance at %p; IC at %p", n, (fleece::InstanceCounted*)n);
        C4Log("VirtCounty instance at %p; IC at %p", v, (fleece::InstanceCountedIn<Virt>*)v);
        REQUIRE(InstanceCounted::liveInstanceCount() == baseInstances + 2);
        c4_dumpInstances();
        delete n;
        delete v;
        REQUIRE(InstanceCounted::liveInstanceCount() == baseInstances);
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
        auto   actor = retained(new TestActor());
        for ( auto& i : t ) {
            i = thread([&actor]() { actor->doot(); });
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
        slice       withPort, unaffected;
        alloc_slice withoutPort;
                    SECTION("Plain") {
            withPort    = "ws://duckduckgo.com:80/search"_sl;
            withoutPort = "ws://duckduckgo.com/search"_sl;
            unaffected  = "ws://duckduckgo.com:4984/search"_sl;
        }

        SECTION("TLS") {
            withPort    = "wss://duckduckgo.com:443/search"_sl;
            withoutPort = "wss://duckduckgo.com/search"_sl;
            unaffected  = "wss://duckduckgo.com:4984/search"_sl;
        }

        alloc_slice asIsWithPort    = transform_url(withPort, URLTransformStrategy::AsIs);
        alloc_slice asIsWithoutPort = transform_url(withoutPort, URLTransformStrategy::AsIs);
        alloc_slice asInUnaffected  = transform_url(unaffected, URLTransformStrategy::AsIs);

        CHECK(asIsWithPort == withPort);
        CHECK(asIsWithoutPort == withoutPort);
        CHECK(asIsWithoutPort.buf == withoutPort.buf);
        CHECK(asInUnaffected == unaffected);

        alloc_slice addPortWithPort    = transform_url(withPort, URLTransformStrategy::AddPort);
        alloc_slice addPortWithoutPort = transform_url(withoutPort, URLTransformStrategy::AddPort);
        alloc_slice addPortUnaffected  = transform_url(unaffected, URLTransformStrategy::AddPort);

        CHECK(addPortWithPort == withPort);
        CHECK(addPortWithoutPort == withPort);
        CHECK(!addPortUnaffected);

        alloc_slice removePortWithPort    = transform_url(withPort, URLTransformStrategy::RemovePort);
        alloc_slice removePortWithoutPort = transform_url(withoutPort, URLTransformStrategy::RemovePort);
        alloc_slice removePortUnaffected  = transform_url(unaffected, URLTransformStrategy::RemovePort);

        CHECK(removePortWithPort == withoutPort);
        CHECK(removePortWithoutPort == withoutPort);
        CHECK(!removePortUnaffected);

        URLTransformStrategy strategy = URLTransformStrategy::AsIs;
        CHECK(++strategy == URLTransformStrategy::AddPort);
        CHECK(++strategy == URLTransformStrategy::RemovePort);
    }
}  // namespace
