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
#include <sqlite3.h>
#include <SQLiteCpp/Exception.h>
#include <errno.h>
#include <string>


namespace cbforest {

    using namespace std;


#pragma mark ERROR CODES, NAMES, etc.


    struct codeMapping { int err; error::Domain domain; int code; };

    // Maps ForestDB errors (fdb_error.h).
    static const codeMapping kForestDBMapping[] = {
        {FDB_RESULT_INVALID_ARGS,       error::CBForest,    error::InvalidParameter},
        {FDB_RESULT_OPEN_FAIL,          error::CBForest,    error::CantOpenFile},
        {FDB_RESULT_NO_SUCH_FILE,       error::CBForest,    error::CantOpenFile},
        {FDB_RESULT_WRITE_FAIL,         error::CBForest,    error::IOError},
        {FDB_RESULT_READ_FAIL,          error::CBForest,    error::IOError},
        {FDB_RESULT_CLOSE_FAIL,         error::CBForest,    error::IOError},
        {FDB_RESULT_COMMIT_FAIL,        error::CBForest,    error::CommitFailed},
        {FDB_RESULT_ALLOC_FAIL,         error::CBForest,    error::MemoryError},
        {FDB_RESULT_KEY_NOT_FOUND,      error::CBForest,    error::NotFound},
        {FDB_RESULT_RONLY_VIOLATION,    error::CBForest,    error::NotWriteable},
        {FDB_RESULT_SEEK_FAIL,          error::CBForest,    error::IOError},
        {FDB_RESULT_FSYNC_FAIL,         error::CBForest,    error::IOError},
        {FDB_RESULT_CHECKSUM_ERROR,     error::CBForest,    error::CorruptData},
        {FDB_RESULT_FILE_CORRUPTION,    error::CBForest,    error::CorruptData},
        {FDB_RESULT_INVALID_HANDLE,     error::CBForest,    error::NotOpen},

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
        {0, /*must end with err=0*/     error::CBForest,    0},
    };

    static const codeMapping kSQLiteMapping[] = {
        {SQLITE_PERM,                   error::CBForest,    error::NotWriteable},
        {SQLITE_BUSY,                   error::CBForest,    error::Busy},
        {SQLITE_LOCKED,                 error::CBForest,    error::Busy},
        {SQLITE_NOMEM,                  error::CBForest,    error::MemoryError},
        {SQLITE_READONLY,               error::CBForest,    error::NotWriteable},
        {SQLITE_IOERR,                  error::CBForest,    error::IOError},
        {SQLITE_CORRUPT,                error::CBForest,    error::CorruptData},
        {SQLITE_FULL,                   error::POSIX,       ENOSPC},
        {SQLITE_CANTOPEN,               error::CBForest,    error::CantOpenFile},
        {SQLITE_NOTADB,                 error::CBForest,    error::CantOpenFile},
        {SQLITE_PERM,                   error::CBForest,    error::NotWriteable},
        {0, /*must end with err=0*/     error::CBForest,    0},
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
    static const char* kDomainNames[] = {"CBForest", "POSIX", "ForestDB", "SQLite"};

    static const char* cbforest_errstr(error::CBForestError code) {
        static const char* kCBForestMessages[error::NumCBForestErrors] = {
            // These must match up with the codes in the declaration of CBForestError
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
        };
        const char *str = nullptr;
        if (code < sizeof(kCBForestMessages)/sizeof(char*))
            str = kCBForestMessages[code];
        if (!str)
            str = "(unknown CBForestError)";
        return str;
    }

    string error::_what(error::Domain domain, int code) noexcept {
        switch (domain) {
            case CBForest:
                return cbforest_errstr((CBForestError)code);
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


    error error::convertRuntimeError(const std::runtime_error &re) {
        auto e = dynamic_cast<const error*>(&re);
        if (e)
            return *e;
        auto se = dynamic_cast<const SQLite::Exception*>(&re);
        if (se)
            return error(SQLite, se->getErrorCode());
        Warn("Caught unexpected C++ runtime_error: %s", re.what());
        return error(CBForest, UnexpectedError);
    }
    
    
    void error::_throw(Domain domain, int code ) {
        CBFDebugAssert(code != 0);
        error err{domain, code};
        if (sWarnOnError) {
            WarnError("CBForest throwing %s error %d: %s",
                      kDomainNames[domain], code, err.what());
        }
        throw err;
    }

    
    void error::_throw(error::CBForestError err) {
        _throw(CBForest, err);
    }


    void error::assertionFailed(const char *fn, const char *file, unsigned line, const char *expr) {
        if (LogLevel > kError || LogCallback == NULL)
            fprintf(stderr, "Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        WarnError("Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        throw error(error::AssertionFailed);
    }


#pragma mark - LOGGING:

    
    static void defaultLogCallback(logLevel level, const char *message) {
        if (!error::sWarnOnError && level >= kError)
            return;
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
