//
//  c4Base.cc
//  CBForest
//
//  Created by Jens Alfke on 8/1/16.
//  Copyright © 2016 Couchbase. All rights reserved.
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

#include "LogInternal.hh"

#include "SQLiteCpp/Exception.h"
#include <ctype.h>

using namespace cbforest;


namespace c4Internal {

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) {
        if (outError) {
            outError->domain = domain;
            outError->code = code;
        }
    }

    void recordException(const std::exception &e, C4Error* outError) {
        static const C4ErrorDomain domainMap[] = {CBForestDomain, POSIXDomain,
                                                  ForestDBDomain, SQLiteDomain};
        error err = error::convertException(e);
        recordError(domainMap[err.domain], err.code, outError);
    }

}


static C4SliceResult stringResult(const char *str) {
    if (str) {
        slice result = alloc_slice(str, strlen(str)).dontFree();
        return {result.buf, result.size};
    } else {
        return {nullptr, 0};
    }
}


C4SliceResult c4error_getMessage(C4Error err) {
    if (err.code == 0) {
        return stringResult(nullptr);
    } else if (err.domain < 1 || err.domain > SQLiteDomain) {
        return stringResult("unknown error domain");
    } else {
        static const error::Domain kDomains[] = {error::CBForest, error::POSIX,
                                                 error::ForestDB, error::SQLite};
        error e(kDomains[err.domain - 1], err.code);
        return stringResult(e.what());
    }
}

char* c4error_getMessageC(C4Error error, char buffer[], size_t bufferSize) {
    C4SliceResult msg = c4error_getMessage(error);
    auto len = std::min(msg.size, bufferSize-1);
    memcpy(buffer, msg.buf, len);
    buffer[len] = '\0';
    free((void*)msg.buf);
    return buffer;
}


int c4_getObjectCount() {
    return InstanceCounted::gObjectCount;
}


bool c4SliceEqual(C4Slice a, C4Slice b) {
    return a == b;
}


void c4slice_free(C4Slice slice) {
    slice.free();
}


static C4LogCallback clientLogCallback;

static void logCallback(logLevel level, const char *message) {
    auto cb = clientLogCallback;
    if (cb)
        cb((C4LogLevel)level, slice(message));
}


void c4log_register(C4LogLevel level, C4LogCallback callback) {
    if (callback) {
        LogLevel = (logLevel)level;
        LogCallback = logCallback;
    } else {
        LogLevel = kNone;
        LogCallback = NULL;
    }
    clientLogCallback = callback;
}


void c4log_setLevel(C4LogLevel level) {
    LogLevel = (logLevel)level;
}
