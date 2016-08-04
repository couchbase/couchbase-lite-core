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

#include "SQLiteCpp/Exception.h"

using namespace cbforest;


namespace c4Internal {
    std::atomic_int InstanceCounted::gObjectCount;

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) {
        if (outError) {
            outError->domain = domain;
            outError->code = code;
        }
    }

    void recordError(const error &e, C4Error* outError) {
        static const C4ErrorDomain domainMap[] = {CBForestDomain, POSIXDomain,
                                                  ForestDBDomain, SQLiteDomain};
        recordError(domainMap[e.domain], e.code, outError);
    }

    void recordException(const std::exception &e, C4Error* outError) {
        auto rterr = dynamic_cast<const std::runtime_error*>(&e);
        if (rterr) {
            recordError(error::convertRuntimeError(*rterr), outError);
        } else {
            Warn("Unexpected C++ \"%s\" exception thrown from CBForest", e.what());
            recordError(C4Domain, kC4ErrorInternalException, outError);
        }
    }

    void recordUnknownException(C4Error* outError) {
        Warn("Unexpected C++ exception thrown from CBForest");
        recordError(C4Domain, kC4ErrorInternalException, outError);
    }
}


C4SliceResult c4error_getMessage(C4Error err) {
    if (err.code == 0)
        return {NULL, 0};

    static const int kDomains[] = {0, error::POSIX, error::ForestDB, 0, error::CBForest, error::SQLite};
    error e((error::Domain)kDomains[err.domain], err.code);
    
    const char *msg = NULL;
    switch (err.domain) {
        case C4Domain:
            switch (err.code) {
                case kC4ErrorInternalException:     msg = "internal exception"; break;
                case kC4ErrorNotInTransaction:      msg = "no transaction is open"; break;
                case kC4ErrorTransactionNotClosed:  msg = "a transaction is still open"; break;
                case kC4ErrorIndexBusy:             msg = "index busy; can't close view"; break;
                case kC4ErrorBadRevisionID:         msg = "invalid revision ID"; break;
                case kC4ErrorCorruptRevisionData:   msg = "corrupt revision data"; break;
                case kC4ErrorCorruptIndexData:      msg = "corrupt view-index data"; break;
                case kC4ErrorAssertionFailed:       msg = "internal assertion failure"; break;
                case kC4ErrorTokenizerError:        msg = "full-text tokenizer error"; break;
                    //FIX: Add other error codes
                default: break;
            }
            break;
        default:
            msg = e.what();
    }

    char buf[100];
    if (!msg) {
        const char* const kDomainNames[4] = {"HTTP", "POSIX", "ForestDB", "CBForest"};
        if (err.domain <= C4Domain)
            sprintf(buf, "unknown %s error %d", kDomainNames[err.domain], err.code);
        else
            sprintf(buf, "bogus C4Error (%d, %d)", err.domain, err.code);
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


void c4log_setLevel(C4LogLevel level) {
    LogLevel = (logLevel)level;
}
