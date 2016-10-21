//
//  c4Base.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/1/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "c4Internal.hh"
#include "c4Database.h"
#include "c4Document.h"
#include "c4Private.h"

#include "Logging.hh"

#include "SQLiteCpp/Exception.h"
#include <ctype.h>
#include <algorithm>

using namespace litecore;


namespace c4Internal {

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) noexcept {
        if (outError) {
            outError->domain = domain;
            outError->code = code;
        }
    }

    void recordException(const exception &e, C4Error* outError) noexcept {
        static const C4ErrorDomain domainMap[] = {LiteCoreDomain, POSIXDomain,
                                                  ForestDBDomain, SQLiteDomain};
        error err = error::convertException(e).standardized();
        recordError(domainMap[err.domain], err.code, outError);
    }


    bool tryCatch(C4Error *error, std::function<void()> fn) noexcept {
        try {
            fn();
            return true;
        } catchError(error);
        return false;
    }


    C4SliceResult stringResult(const char *str) {
        if (str) {
            slice result = alloc_slice(str, strlen(str)).dontFree();
            return {result.buf, result.size};
        } else {
            return {nullptr, 0};
        }
    }

}


C4SliceResult c4error_getMessage(C4Error err) noexcept {
    if (err.code == 0) {
        return stringResult(nullptr);
    } else if (err.domain < 1 || err.domain > SQLiteDomain) {
        return stringResult("unknown error domain");
    } else {
        static constexpr error::Domain kDomains[] = {error::LiteCore, error::POSIX,
                                                     error::ForestDB, error::SQLite};
        error e(kDomains[err.domain - 1], err.code);
        return stringResult(e.what());
    }
}

char* c4error_getMessageC(C4Error error, char buffer[], size_t bufferSize) noexcept {
    C4SliceResult msg = c4error_getMessage(error);
    auto len = min(msg.size, bufferSize-1);
    if (msg.buf)
        memcpy(buffer, msg.buf, len);
    buffer[len] = '\0';
    free((void*)msg.buf);
    return buffer;
}


int c4_getObjectCount() noexcept {
    return InstanceCounted::gObjectCount;
}


bool c4SliceEqual(C4Slice a, C4Slice b) noexcept {
    return a == b;
}


void c4slice_free(C4SliceResult slice) noexcept {
    free((void*)slice.buf);
}


static C4LogCallback clientLogCallback;

static void logCallback(const LogDomain &domain, LogLevel level, const char *message) {
    auto cb = clientLogCallback;
    if (cb)
        cb((C4LogLevel)level, slice(message));
}


void c4log_register(C4LogLevel level, C4LogCallback callback) noexcept {
    if (callback) {
        LogDomain::MinLevel = (LogLevel)level;
        LogDomain::Callback = logCallback;
    } else {
        LogDomain::MinLevel = LogLevel::None;
        LogDomain::Callback = nullptr;
    }
    clientLogCallback = callback;
}


void c4log_setLevel(C4LogLevel level) noexcept {
    LogDomain::MinLevel = (LogLevel)level;
}

void c4log_warnOnErrors(bool warn) noexcept {
    error::sWarnOnError = warn;
}
