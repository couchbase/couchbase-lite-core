//
//  c4Base.cc
//  CBForest
//
//  Created by Jens Alfke on 8/1/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "LogInternal.hh"

using namespace cbforest;


namespace c4Internal {
    std::atomic_int InstanceCounted::gObjectCount;

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) {
        if (outError) {
            if (domain == ForestDBDomain && code <= -1000)   // custom CBForest errors (Error.hh)
                domain = C4Domain;
            outError->domain = domain;
            outError->code = code;
        }
    }

    void recordHTTPError(int httpStatus, C4Error* outError) {
        recordError(HTTPDomain, httpStatus, outError);
    }

    void recordError(const error &e, C4Error* outError) {
        static const C4ErrorDomain domainMap[] = {CBForestDomain, POSIXDomain, HTTPDomain,
                                                  ForestDBDomain, SQLiteDomain};
        recordError(domainMap[e.domain], e.code, outError);
    }

    void recordException(const std::exception &e, C4Error* outError) {
        Warn("Unexpected C++ \"%s\" exception thrown from CBForest", e.what());
        recordError(C4Domain, kC4ErrorInternalException, outError);
    }

    void recordUnknownException(C4Error* outError) {
        Warn("Unexpected C++ exception thrown from CBForest");
        recordError(C4Domain, kC4ErrorInternalException, outError);
    }

    void throwHTTPError(int status) {
        error::_throwHTTPStatus(status);
    }
}


C4SliceResult c4error_getMessage(C4Error error) {
    if (error.code == 0)
        return {NULL, 0};
    
    const char *msg = NULL;
    switch (error.domain) {
        case ForestDBDomain:
            msg = fdb_error_msg((fdb_status)error.code);
            if (strcmp(msg, "unknown error") == 0)
                msg = NULL;
            break;
        case POSIXDomain:
            msg = strerror(error.code);
            break;
        case HTTPDomain:
            switch (error.code) {
                case kC4HTTPBadRequest: msg = "invalid parameter"; break;
                case kC4HTTPNotFound:   msg = "not found"; break;
                case kC4HTTPConflict:   msg = "conflict"; break;
                case kC4HTTPGone:       msg = "gone"; break;

                default: break;
            }
            break;
        case CBForestDomain:
            msg = cbforest::error(cbforest::error::CBForest, error.code).what();
            break;
        case SQLiteDomain:
            msg = cbforest::error(cbforest::error::SQLite, error.code).what();
            break;
        case C4Domain:
            switch (error.code) {
                case kC4ErrorInternalException:     msg = "internal exception"; break;
                case kC4ErrorNotInTransaction:      msg = "no transaction is open"; break;
                case kC4ErrorTransactionNotClosed:  msg = "a transaction is still open"; break;
                case kC4ErrorIndexBusy:             msg = "index busy; can't close view"; break;
                case kC4ErrorBadRevisionID:         msg = "invalid revision ID"; break;
                case kC4ErrorCorruptRevisionData:   msg = "corrupt revision data"; break;
                case kC4ErrorCorruptIndexData:      msg = "corrupt view-index data"; break;
                case kC4ErrorAssertionFailed:       msg = "internal assertion failure"; break;
                case kC4ErrorTokenizerError:        msg = "full-text tokenizer error"; break;
                default: break;
            }
    }

    char buf[100];
    if (!msg) {
        const char* const kDomainNames[4] = {"HTTP", "POSIX", "ForestDB", "CBForest"};
        if (error.domain <= C4Domain)
            sprintf(buf, "unknown %s error %d", kDomainNames[error.domain], error.code);
        else
            sprintf(buf, "bogus C4Error (%d, %d)", error.domain, error.code);
        msg = buf;
    }

    slice result = alloc_slice(msg, strlen(msg)).dontFree();
    return {result.buf, result.size};
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
