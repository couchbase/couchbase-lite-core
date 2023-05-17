//
// Error.hh
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include <stdexcept>
#include <atomic>
#include <functional>
#include <memory>

#undef check

namespace fleece {
    class Backtrace;
}

namespace litecore {

    /** Most API calls can throw this. */
    struct error : public std::runtime_error {
        enum Domain {
            LiteCore = 1,  // See LiteCoreError enum, below
            POSIX,         // See <errno.h>
            SQLite,        // See <sqlite3.h>
            Fleece,        // See FleeceException.h
            Network,       // See NetworkError enum in WebSocketInterface.hh
            WebSocket,     // See WebSocketInterface.h
            MbedTLS,       // See mbedtls/error.h

            // Add new domain here.
            // You MUST add a name string to kDomainNames in Error.cc!
            // You MUST add a corresponding domain to C4ErrorDomain in c4Base.h!
            NumDomainsPlus1
        };

        // Error codes in LiteCore domain:
        enum LiteCoreError {
            AssertionFailed = 1,
            Unimplemented,
            UnsupportedEncryption,
            BadRevisionID,
            CorruptRevisionData,
            NotOpen,
            NotFound,
            Conflict,
            InvalidParameter,
            UnexpectedError,
            CantOpenFile,
            IOError,
            MemoryError,
            NotWriteable,
            CorruptData,
            Busy,
            NotInTransaction,
            TransactionNotClosed,
            UnsupportedOperation,
            NotADatabaseFile,
            WrongFormat,
            CryptoError,
            InvalidQuery,
            NoSuchIndex,
            InvalidQueryParam,
            RemoteError,
            DatabaseTooOld,
            DatabaseTooNew,
            BadDocID,
            CantUpgradeDatabase,
            DeltaBaseUnknown,
            CorruptDelta,

            // Add new codes here. You MUST add messages to kLiteCoreMessages!
            // You MUST add corresponding kC4Err codes to the enum in C4Base.h!

            NumLiteCoreErrorsPlus1
        };

        //---- Data members:
        Domain const                       domain;
        int const                          code;
        std::shared_ptr<fleece::Backtrace> backtrace;

        error(Domain, int code);
        error(error::Domain, int code, const std::string& what);
        error(Domain, int code, const std::string& what, std::shared_ptr<fleece::Backtrace> btrace);

        explicit error(LiteCoreError e) : error(LiteCore, e) {}

        error& operator=(const error& e);

        void captureBacktrace(unsigned skipFrames = 0);

        [[noreturn]] void _throw(unsigned skipFrames = 0);

        /** Returns an equivalent error in the LiteCore or POSIX domain. */
        [[nodiscard]] error standardized() const;

        [[nodiscard]] bool isUnremarkable() const;

        /** Returns the error equivalent to a given runtime_error. Uses RTTI to discover if the
            error is already an `error` instance; otherwise tries to convert some other known
            exception types like SQLite::Exception. */
        static error convertRuntimeError(const std::runtime_error&);
        static error convertException(const std::exception&);

        /** Static version of the standard `what` method. */
        static std::string _what(Domain, int code) noexcept;

        static const char* nameOfDomain(Domain) noexcept;

        /** Constructs and throws an error. */
        [[noreturn]] static void _throw(Domain d, int c);
        [[noreturn]] static void _throw(LiteCoreError);
        [[noreturn]] static void _throwErrno();
        [[noreturn]] static void _throw(LiteCoreError, const char* msg, ...) __printflike(2, 3);
        [[noreturn]] static void _throwErrno(const char* msg, ...) __printflike(1, 2);

        /** Throws an assertion failure exception. Called by the Assert() macro. */
        [[noreturn]] static void assertionFailed(const char* func, const char* file, unsigned line, const char* expr,
                                                 const char* message = nullptr, ...) __printflike(5, 6);

        static void setNotableExceptionHook(std::function<void()> hook);

        static bool sWarnOnError;
        static bool sCaptureBacktraces;
    };

    static inline bool operator==(const error& a, const error& b) noexcept {
        return a.domain == b.domain && a.code == b.code;
    }

    static inline bool operator==(const error& a, error::LiteCoreError code) noexcept {
        return a.domain == error::LiteCore && a.code == code;
    }

// Like C assert() but throws an exception instead of aborting
#ifdef __FILE_NAME__
#    define Assert(e, ...)                                                                                             \
        (_usuallyFalse(!(e)) ? litecore::error::assertionFailed(__func__, __FILE_NAME__, __LINE__, #e, ##__VA_ARGS__)  \
                             : (void)0)
#else
#    define Assert(e, ...)                                                                                             \
        (_usuallyFalse(!(e)) ? litecore::error::assertionFailed(__func__, __FILE__, __LINE__, #e, ##__VA_ARGS__)       \
                             : (void)0)
#endif

// DebugAssert is removed from release builds; use when 'e' test is too expensive
#ifndef DEBUG
#    define DebugAssert(e, ...)                                                                                        \
        do {                                                                                                           \
        } while ( 0 )
#else
#    define DebugAssert(e, ...) Assert(e, ##__VA_ARGS__)
#endif

}  // namespace litecore
