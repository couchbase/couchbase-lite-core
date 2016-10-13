//
//  Error.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 6/15/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#pragma once

#include "Base.hh"
#include <stdexcept>

#undef check

namespace litecore {

    /** Most API calls can throw this. */
    struct error : public std::runtime_error {

        enum Domain {
            LiteCore,
            POSIX,
            ForestDB,
            SQLite,
        };

        // Error codes in LiteCore domain:
        enum LiteCoreError {
            AssertionFailed = 1,
            Unimplemented,
            NoSequences,
            UnsupportedEncryption,
            NoTransaction,
            BadRevisionID,
            BadVersionVector,
            CorruptRevisionData,
            CorruptIndexData,
            TokenizerError, // can't create text tokenizer for FTS
            NotOpen,
            NotFound,
            Deleted,
            Conflict,
            InvalidParameter,
            DatabaseError,
            UnexpectedError,
            CantOpenFile,
            IOError,
            CommitFailed,
            MemoryError,
            NotWriteable,
            CorruptData,
            Busy,
            NotInTransaction,
            TransactionNotClosed,
            IndexBusy,
            UnsupportedOperation,
            NotADatabaseFile,
            WrongFormat,                // data exists but not in format requested, or data given is in unusable format
            CryptoError,
            InvalidQuery,
            
            NumLiteCoreErrors
        };

        Domain const domain;
        int const code;

        error (Domain d, int c );
        explicit error (LiteCoreError e)     :error(LiteCore, e) {}

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

        /** Constructs and throws an error. */
        [[noreturn]] static void _throw(Domain d, int c );
        [[noreturn]] static void _throw(LiteCoreError);
        [[noreturn]] static void _throwErrno();

        /** Throws an assertion failure exception. Called by the Assert() macro. */
        [[noreturn]] static void assertionFailed(const char *func, const char *file, unsigned line,
                                                 const char *expr,
                                                 const char *message =nullptr);

        static void logBacktrace(unsigned skipFrames =0);

        static bool sWarnOnError;
    };


// Like C assert() but throws an exception instead of aborting
#define	Assert(e, ...) \
    (_usuallyFalse(!(e)) ? litecore::error::assertionFailed(__func__, __FILE__, __LINE__, #e, ##__VA_ARGS__) \
                         : (void)0)

// DebugAssert is removed from release builds; use when 'e' test is too expensive
#ifdef NDEBUG
#define DebugAssert(e, ...)   do{ }while(0)
#else
#define DebugAssert(e, ...)   Assert(e, ##__VA_ARGS__)
#endif

}
