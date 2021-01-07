//
// Error.hh
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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

#pragma once

#include "Base.hh"
#include <stdexcept>
#include <atomic>
#include <functional>

#undef check

namespace litecore {

    /** Most API calls can throw this. */
    struct error : public std::runtime_error {

        enum Domain {
            LiteCore = 1,       // See LiteCoreError enum, below
            POSIX,              // See <errno.h>
            SQLite,             // See <sqlite3.h>
            Fleece,             // See FleeceException.h
            Network,            // See NetworkError enum in WebSocketInterface.hh
            WebSocket,          // See WebSocketInterface.h
            MbedTLS,            // See mbedtls/error.h

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

        Domain const domain;
        int const code;

        error (Domain, int code );
        error(error::Domain, int code, const std::string &what);
        explicit error (LiteCoreError e)     :error(LiteCore, e) {}

        error& operator= (const error &e);

        [[noreturn]] void _throw();

        /** Returns an equivalent error in the LiteCore or POSIX domain. */
        error standardized() const;

        bool isUnremarkable() const;

        /** Returns the error equivalent to a given runtime_error. Uses RTTI to discover if the
            error is already an `error` instance; otherwise tries to convert some other known
            exception types like SQLite::Exception. */
        static error convertRuntimeError(const std::runtime_error&);
        static error convertException(const std::exception&);

        /** Static version of the standard `what` method. */
        static std::string _what(Domain, int code) noexcept;

        static const char* nameOfDomain(Domain) noexcept;

        /** Constructs and throws an error. */
        [[noreturn]] static void _throw(Domain d, int c );
        [[noreturn]] static void _throw(LiteCoreError);
        [[noreturn]] static void _throwErrno();
        [[noreturn]] static void _throw(LiteCoreError, const char *msg, ...) __printflike(2,3);

        /** Throws an assertion failure exception. Called by the Assert() macro. */
        [[noreturn]] static void assertionFailed(const char *func, const char *file, unsigned line,
                                                 const char *expr,
                                                 const char *message =nullptr, ...);

        static std::string backtrace(unsigned skipFrames =0);

        static void setNotableExceptionHook(std::function<void()> hook);

        static bool sWarnOnError;
    };


// Like C assert() but throws an exception instead of aborting
#define	Assert(e, ...) \
    (_usuallyFalse(!(e)) ? litecore::error::assertionFailed(__func__, __FILE__, __LINE__, #e, ##__VA_ARGS__) \
                         : (void)0)

// DebugAssert is removed from release builds; use when 'e' test is too expensive
#ifndef DEBUG
#define DebugAssert(e, ...)   do{ }while(0)
#else
#define DebugAssert(e, ...)   Assert(e, ##__VA_ARGS__)
#endif

}
