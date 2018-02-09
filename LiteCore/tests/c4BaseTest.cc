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
#include "c4Private.h"
#include "catch.hpp"


// NOTE: These tests have to be in the C++ tests target, not the C tests, because they use internal
// LiteCore symbols that aren't exported by the dynamic library.


static string result2string(C4StringResult result) {
    return string((char*)result.buf, result.size);
}


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
        C4StringResult message = c4error_getMessage(errors[i]);
        string messageStr = result2string(message);
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
    C4StringResult message = c4error_getMessage(error);
    string messageStr = result2string(message);
    CHECK(messageStr == "Oops");
}
