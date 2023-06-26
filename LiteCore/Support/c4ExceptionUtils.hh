//
// c4ExceptionUtils.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Private.h"
#include "fleece/PlatformCompat.hh"  // for NOINLINE, ALWAYS_INLINE
#include <atomic>

namespace litecore {

    // Utilities for handling C++ exceptions and mapping them to C4Errors.


    /** Clears a C4Error back to empty. */
    static inline void clearError(C4Error* outError) noexcept {
        if ( outError ) outError->code = 0;
    }

/** Macro to substitute for a regular 'catch' block, that saves any exception in OUTERR. */
#define catchError(OUTERR)                                                                                             \
    catch ( ... ) {                                                                                                    \
        C4Error::fromCurrentException(OUTERR);                                                                         \
    }

    /** Macro to substitute for a regular 'catch' block, that just logs a warning. */
#ifdef _MSC_VER
#    define catchAndWarn()                                                                                             \
        catch ( ... ) {                                                                                                \
            C4Error::warnCurrentException(__FUNCSIG__);                                                                \
        }
#else
#    define catchAndWarn()                                                                                             \
        catch ( ... ) {                                                                                                \
            C4Error::warnCurrentException(__PRETTY_FUNCTION__);                                                        \
        }
#endif

/** Precondition check. If `TEST` is not truthy, throws InvalidParameter. */
#define AssertParam(TEST, MSG) ((TEST) || (C4Error::raise(LiteCoreDomain, kC4ErrorInvalidParameter, MSG), false))

    // Calls the function `fn`, returning its return value.
    // If `fn` throws an exception, it catches the exception, stores it into `outError`,
    // and returns a default 0/nullptr/false value.
    template <typename RESULT, typename LAMBDA>
    ALWAYS_INLINE RESULT tryCatch(C4Error* outError, LAMBDA fn) noexcept {
        try {
            return fn();
        }
        catchError(outError);
        return RESULT();  // this will be 0, nullptr, false, etc.
    }

    // Calls the function and returns true.
    // If `fn` throws an exception, it catches the exception, stores it into `outError`,
    // and returns false.
    template <typename LAMBDA>
    ALWAYS_INLINE bool tryCatch(C4Error* outError, LAMBDA fn) noexcept {
        try {
            fn();
            return true;
        }
        catchError(outError);
        return false;
    }

    // RAII utility to suppress reporting C++ exceptions (or breaking at them, in the Xcode debugger.)
    // Declare an instance when testing something that's expected to throw an exception internally.
    struct ExpectingExceptions {
        ExpectingExceptions() {
            ++gC4ExpectExceptions;
            c4log_warnOnErrors(false);
        }

        ~ExpectingExceptions() {
            if ( --gC4ExpectExceptions == 0 ) c4log_warnOnErrors(true);
        }
    };

}  // namespace litecore
