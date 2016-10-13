//
//  Error.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 3/4/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Error.hh"
#include "LogInternal.hh"
#include "forestdb.h"
#include <sqlite3.h>
#include <SQLiteCpp/Exception.h>
#include <errno.h>
#include <string>

#if __ANDROID__
#include <android/log.h>
#elif defined(_MSC_VER)
#include "asprintf.h"
#endif

#if __APPLE__   // For logBacktrace:
#include <execinfo.h>   // Not available in Linux or Windows?
#include <unistd.h>
#include <cxxabi.h>
#endif

namespace litecore {

    using namespace std;


#pragma mark ERROR CODES, NAMES, etc.


    struct codeMapping { int err; error::Domain domain; int code; };

    // Maps ForestDB errors (fdb_error.h).
    static const codeMapping kForestDBMapping[] = {
        {FDB_RESULT_INVALID_ARGS,       error::LiteCore,    error::InvalidParameter},
        {FDB_RESULT_OPEN_FAIL,          error::LiteCore,    error::CantOpenFile},
        {FDB_RESULT_NO_SUCH_FILE,       error::LiteCore,    error::CantOpenFile},
        {FDB_RESULT_WRITE_FAIL,         error::LiteCore,    error::IOError},
        {FDB_RESULT_READ_FAIL,          error::LiteCore,    error::IOError},
        {FDB_RESULT_CLOSE_FAIL,         error::LiteCore,    error::IOError},
        {FDB_RESULT_COMMIT_FAIL,        error::LiteCore,    error::CommitFailed},
        {FDB_RESULT_ALLOC_FAIL,         error::LiteCore,    error::MemoryError},
        {FDB_RESULT_KEY_NOT_FOUND,      error::LiteCore,    error::NotFound},
        {FDB_RESULT_RONLY_VIOLATION,    error::LiteCore,    error::NotWriteable},
        {FDB_RESULT_SEEK_FAIL,          error::LiteCore,    error::IOError},
        {FDB_RESULT_FSYNC_FAIL,         error::LiteCore,    error::IOError},
        {FDB_RESULT_CHECKSUM_ERROR,     error::LiteCore,    error::CorruptData},
        {FDB_RESULT_FILE_CORRUPTION,    error::LiteCore,    error::CorruptData},
        {FDB_RESULT_INVALID_HANDLE,     error::LiteCore,    error::NotOpen},
        {FDB_RESULT_NO_DB_HEADERS,      error::LiteCore,    error::NotADatabaseFile},

        {FDB_RESULT_EPERM,              error::POSIX,       EPERM},
        {FDB_RESULT_EIO,                error::POSIX,       EIO},
        {FDB_RESULT_ENXIO,              error::POSIX,       ENXIO},
        {FDB_RESULT_ENOMEM,             error::POSIX,       ENOMEM},
        {FDB_RESULT_EACCESS,            error::POSIX,       EACCES},
        {FDB_RESULT_EFAULT,             error::POSIX,       EFAULT},
        {FDB_RESULT_EEXIST,             error::POSIX,       EEXIST},
        {FDB_RESULT_ENODEV,             error::POSIX,       ENODEV},
        {FDB_RESULT_ENOTDIR,            error::POSIX,       ENOTDIR},
        {FDB_RESULT_EISDIR,             error::POSIX,       EISDIR},
        {FDB_RESULT_EINVAL,             error::POSIX,       EINVAL},
        {FDB_RESULT_ENFILE,             error::POSIX,       ENFILE},
        {FDB_RESULT_EMFILE,             error::POSIX,       EMFILE},
        {FDB_RESULT_EFBIG,              error::POSIX,       EFBIG},
        {FDB_RESULT_ENOSPC,             error::POSIX,       ENOSPC},
        {FDB_RESULT_EROFS,              error::POSIX,       EROFS},
        {FDB_RESULT_EOPNOTSUPP,         error::POSIX,       EOPNOTSUPP},
        {FDB_RESULT_ENOBUFS,            error::POSIX,       ENOBUFS},
        {FDB_RESULT_ELOOP,              error::POSIX,       ELOOP},
        {FDB_RESULT_ENAMETOOLONG,       error::POSIX,       ENAMETOOLONG},
        {FDB_RESULT_EOVERFLOW,          error::POSIX,       EOVERFLOW},
        {FDB_RESULT_EAGAIN,             error::POSIX,       EAGAIN},
        {0, /*must end with err=0*/     error::LiteCore,    0},
    };

    static const codeMapping kPOSIXMapping[] = {
        {ENOENT,                        error::LiteCore,    error::NotFound},
    };

    static const codeMapping kSQLiteMapping[] = {
        {SQLITE_PERM,                   error::LiteCore,    error::NotWriteable},
        {SQLITE_BUSY,                   error::LiteCore,    error::Busy},
        {SQLITE_LOCKED,                 error::LiteCore,    error::Busy},
        {SQLITE_NOMEM,                  error::LiteCore,    error::MemoryError},
        {SQLITE_READONLY,               error::LiteCore,    error::NotWriteable},
        {SQLITE_IOERR,                  error::LiteCore,    error::IOError},
        {SQLITE_CORRUPT,                error::LiteCore,    error::CorruptData},
        {SQLITE_FULL,                   error::POSIX,       ENOSPC},
        {SQLITE_CANTOPEN,               error::LiteCore,    error::CantOpenFile},
        {SQLITE_NOTADB,                 error::LiteCore,    error::NotADatabaseFile},
        {SQLITE_PERM,                   error::LiteCore,    error::NotWriteable},
        {0, /*must end with err=0*/     error::LiteCore,    0},
    };
    //TODO: Map the SQLite 'extended error codes' that give more detail about file errors

    static bool mapError(error::Domain &domain, int &code, const codeMapping table[]) {
        for (const codeMapping *row = &table[0]; row->err != 0; ++row) {
            if (row->err == code) {
                domain = row->domain;
                code = row->code;
                return true;
            }
        }
        return false;
    }


    // Indexed by C4ErrorDomain
    static const char* kDomainNames[] = {"LiteCore", "POSIX", "ForestDB", "SQLite"};

    static const char* litecore_errstr(error::LiteCoreError code) {
        static const char* kLiteCoreMessages[error::NumLiteCoreErrors] = {
            // These must match up with the codes in the declaration of LiteCoreError
            "no error", // 0
            "assertion failed",
            "unimplemented function called",
            "database doesn't support sequences",
            "unsupported encryption algorithm",
            "call must be made in a transaction",
            "bad revision ID",
            "bad version vector",
            "corrupt revision data",
            "corrupt index",
            "text tokenizer error",
            "database not open",
            "not found",
            "deleted",
            "conflict",
            "invalid parameter",
            "database error",
            "unexpected exception",
            "can't open file",
            "file I/O error",
            "commit failed",
            "memory allocation failed",
            "not writeable",
            "file data is corrupted",
            "database busy/locked",
            "must be called during a transaction",
            "transaction not closed",
            "index busy; can't close view",
            "unsupported operation for this database type",
            "file is not a database (or encryption key is invalid/missing)",
            "file/data is not in the requested format",
            "encryption/decryption error",
            "query syntax error",
        };
        const char *str = nullptr;
        if (code < sizeof(kLiteCoreMessages)/sizeof(char*))
            str = kLiteCoreMessages[code];
        if (!str)
            str = "(unknown LiteCoreError)";
        return str;
    }

    string error::_what(error::Domain domain, int code) noexcept {
        switch (domain) {
            case LiteCore:
                return litecore_errstr((LiteCoreError)code);
            case POSIX:
                return strerror(code);
            case ForestDB:
                return fdb_error_msg((fdb_status)code);
            case SQLite:
                return sqlite3_errstr(code);
            default:
                return "unknown error domain";
        }
    }


#pragma mark - ERROR CLASS:


    bool error::sWarnOnError = true;

    
    error::error (error::Domain d, int c )
    :runtime_error(_what(d, c)),
    domain(d),
    code(c)
    { }


    error error::standardized() const {
        Domain d = domain;
        int c = code;
        switch (domain) {
            case POSIX:
                mapError(d, c, kPOSIXMapping);
                break;
            case ForestDB:
                mapError(d, c, kForestDBMapping);
                break;
            case SQLite:
                mapError(d, c, kSQLiteMapping);
                break;
            default:
                return *this;
        }
        return error(d, c);
    }


    static error unexpectedException(const std::exception &x) {
        // Get the actual exception class name using RTTI.
        // Unmangle it by skipping class name prefix like "St12" (may be compiler dependent)
        const char *name =  typeid(x).name();
        while (isalpha(*name)) ++name;
        while (isdigit(*name)) ++name;
        Warn("Caught unexpected C++ %s(\"%s\")", name, x.what());
        return error(error::LiteCore, error::UnexpectedError);
    }


    error error::convertRuntimeError(const std::runtime_error &re) {
        auto e = dynamic_cast<const error*>(&re);
        if (e)
            return *e;
        auto se = dynamic_cast<const SQLite::Exception*>(&re);
        if (se)
            return error(SQLite, se->getErrorCode());
        return unexpectedException(re);
    }

    error error::convertException(const std::exception &x) {
        auto re = dynamic_cast<const std::runtime_error*>(&x);
        if (re)
            return convertRuntimeError(*re);
        return unexpectedException(x);
    }


    bool error::isUnremarkable() const {
        if (code == 0)
            return true;
        switch (domain) {
            case LiteCore:
                return code == NotFound || code == Deleted;
            case POSIX:
                return code == ENOENT;
            case ForestDB:
                return code == FDB_RESULT_KEY_NOT_FOUND || code == FDB_RESULT_NO_DB_HEADERS;
            default:
                return false;
        }
    }

    
    void error::_throw(Domain domain, int code ) {
        CBFDebugAssert(code != 0);
        error err{domain, code};
        if (sWarnOnError && !err.isUnremarkable()) {
            WarnError("LiteCore throwing %s error %d: %s",
                      kDomainNames[domain], code, err.what());
            if (LogLevel <= kError)
                logBacktrace(1);
        }
        throw err;
    }

    
    void error::_throw(error::LiteCoreError err) {
        _throw(LiteCore, err);
    }

    void error::_throwErrno() {
        _throw(POSIX, errno);
    }



    void error::assertionFailed(const char *fn, const char *file, unsigned line, const char *expr) {
        if (LogLevel > kError || LogCallback == nullptr)
            fprintf(stderr, "Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        WarnError("Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        if (LogLevel <= kError)
            logBacktrace(1);
        throw error(error::AssertionFailed);
    }


    /*static*/ void error::logBacktrace(unsigned skip) {
#if __APPLE__
        ++skip;     // skip the logBacktrace frame itself
        void* addrs[50];
        int n = backtrace(addrs, 50) - skip;
        if (n <= 0)
            return;
        char** lines = backtrace_symbols(&addrs[skip], n);
        char* unmangled = nullptr;
        size_t unmangledLen = 0;

        for (int i = 0; i < n; ++i) {
            char library[101], functionBuf[201];
            size_t pc;
            int offset;
            if (sscanf(lines[i], "%*d %100s %zi %200s + %i",
                       library, &pc, functionBuf, &offset) == 4) {
                const char *function = functionBuf;
                int status;
                unmangled = abi::__cxa_demangle(function, unmangled, &unmangledLen, &status);
                if (unmangled && status == 0)
                    function = unmangled;
                fprintf(stderr, "%2d  %-25s %s + %d\n", i, library, function, offset);
            } else {
                fprintf(stderr, "%s\n", lines[i]);
            }
        }
        free(unmangled);
        free(lines);
#endif
    }


#pragma mark - LOGGING:

    
    static void defaultLogCallback(logLevel level, const char *message) {
        if (!error::sWarnOnError && level >= kError)
            return;
#ifdef __ANDROID__
        static const int kLevels[4] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        __android_log_write(kLevels[level], "LiteCore", message);
#else
        static const char* kLevelNames[4] = {"debug", "info", "WARNING", "ERROR"};
        fprintf(stderr, "LiteCore %s: %s\n", kLevelNames[level], message);
#endif
    }

    logLevel LogLevel = kWarning;
    void (*LogCallback)(logLevel, const char *message) = &defaultLogCallback;

    void _Log(logLevel level, const char *message, ...) {
        if (LogLevel <= level && LogCallback != nullptr) {
            va_list args;
            va_start(args, message);
            char *formatted = nullptr;
            vasprintf(&formatted, message, args);
            va_end(args);
            LogCallback(level, formatted);
        }
    }

}
