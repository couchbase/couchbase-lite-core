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
#include "InstanceCounted.hh"
#include "catch.hpp"
#include "Actor.hh"
#include "URLTransformer.hh"
#ifdef WIN32
#include <winerror.h>
#endif


using namespace fleece;
using namespace litecore::repl;


// NOTE: These tests have to be in the C++ tests target, not the C tests, because they use internal
// LiteCore symbols that aren't exported by the dynamic library.


TEST_CASE("C4Error messages") {
    C4Error errors[100];
    for (int i = 0; i < 100; i++) {
        char message[100];
        sprintf(message, "Error number %d", 1000+i);
        c4Internal::recordError(LiteCoreDomain, 1000+i, message, &errors[i]);
    }
    for (int i = 0; i < 100; i++) {
        CHECK(errors[i].domain == LiteCoreDomain);
        CHECK(errors[i].code == 1000+i);
        alloc_slice message = c4error_getMessage(errors[i]);
        string messageStr = string(message);
        if (i >= (100 - kMaxErrorMessagesToSave)) {
            // The last 10 C4Errors generated will have their custom messages:
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
