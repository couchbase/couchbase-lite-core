//
//  Error.cc
//  CBForest
//
//  Created by Jens Alfke on 3/4/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Error.hh"
#include "LogInternal.hh"
#include "forestdb.h"
#include <string>


namespace cbforest {

    static const char* kDomainNames[] = {"CBForest", "POSIX", "HTTP", "ForestDB", "SQLite"};

    const char* error::what() const noexcept {
        switch (domain) {
            case ForestDB:
                return fdb_error_msg((fdb_status)code);
            default:
                return "cbforest::error?";
        }
    }

    
    void error::_throw(Domain domain, int code ) {
        WarnError("CBForest throwing error (%s, %d)", kDomainNames[domain], code);
        throw error{domain, code};
    }

    
    void error::_throw(error::CBForestError err) {
        _throw(CBForest, err);
    }

    void error::_throwHTTPStatus(int status) {
        _throw(HTTP, status);
    }


    void error::assertionFailed(const char *fn, const char *file, unsigned line, const char *expr) {
        if (LogLevel > kError || LogCallback == NULL)
            fprintf(stderr, "Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        WarnError("Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        throw error(error::AssertionFailed);
    }


#pragma mark - LOGGING:

    
    static void defaultLogCallback(logLevel level, const char *message) {
        static const char* kLevelNames[4] = {"debug", "info", "WARNING", "ERROR"};
        fprintf(stderr, "CBForest %s: %s\n", kLevelNames[level], message);
    }

    logLevel LogLevel = kWarning;
    void (*LogCallback)(logLevel, const char *message) = &defaultLogCallback;

    void _Log(logLevel level, const char *message, ...) {
        if (LogLevel <= level && LogCallback != NULL) {
            va_list args;
            va_start(args, message);
            char *formatted = NULL;
            vasprintf(&formatted, message, args);
            va_end(args);
            LogCallback(level, formatted);
        }
    }
    
}
