//
// c4ExceptionUtils.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
#include "c4Base.h"
#include "PlatformCompat.hh"    // for NOINLINE, ALWAYS_INLINE
#include <atomic>
#include <exception>


extern "C" CBL_CORE_API std::atomic_int gC4ExpectExceptions; 

namespace litecore {

    // Utilities for handling C++ exceptions and mapping them to C4Errors.


    /** Clears a C4Error back to empty. */
    static inline void clearError(C4Error* outError) noexcept {if (outError) outError->code = 0;}


    /** Macro to substitute for a regular 'catch' block, that saves any exception in OUTERR. */
    #define catchError(OUTERR) \
        catch (...) { \
            C4Error::fromCurrentException(OUTERR); \
        }

    /** Macro to substitute for a regular 'catch' block, that completely ignores exceptions. */
    #define catchAndIgnore() \
        catch (...) { }

    /** Precondition check. If `TEST` is not truthy, throws InvalidParameter. */
    #define AssertParam(TEST, MSG) \
        ((TEST) || (error::_throw(error::InvalidParameter, MSG), false))


    // Calls the function `fn`, returning its return value.
    // If `fn` throws an exception, it catches the exception, stores it into `outError`,
    // and returns a default 0/nullptr/false value.
    template <typename RESULT, typename LAMBDA>
    ALWAYS_INLINE RESULT tryCatch(C4Error *outError, LAMBDA fn) noexcept {
        try {
            return fn();
        } catchError(outError);
        return RESULT(); // this will be 0, nullptr, false, etc.
    }


    // Calls the function and returns true.
    // If `fn` throws an exception, it catches the exception, stores it into `outError`,
    // and returns false.
    template <typename LAMBDA>
    ALWAYS_INLINE bool tryCatch(C4Error *outError, LAMBDA fn) noexcept {
        try {
            fn();
            return true;
        } catchError(outError);
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
            if (--gC4ExpectExceptions == 0)
                c4log_warnOnErrors(true);
        }
    };

}
