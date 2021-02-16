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
#include <exception>

namespace c4Internal {

    // Utilities for handling C++ exceptions and mapping them to C4Errors.
    //
    // These can't be exported from the LiteCore dynamic library because they use C++ classes and
    // calling conventions.
    //
    // Implementation is in c4Error.cc.

    /** Converts a C++ exception to a C4Error and stores it in `outError` if it's not NULL. */
    NOINLINE void recordException(const std::exception &e, C4Error* outError) noexcept;

    /** Converts the _currently caught_ C++ exception to a C4Error. Use only in a `catch` block. */
    NOINLINE void recordException(C4Error* outError) noexcept;

    /** Clears a C4Error back to empty. */
    static inline void clearError(C4Error* outError) noexcept {if (outError) outError->code = 0;}

    /** Macro to substitute for a regular 'catch' block, that saves any exception in OUTERR. */
    #define catchError(OUTERR) \
        catch (...) { \
            c4Internal::recordException(OUTERR); \
        }

    #define catchExceptions() \
        catch (...) { }

    /** Precondition check. If `TEST` is not truthy, stores an invalid-parameter error in `OUTERROR`
        and returns false. */
    #define checkParam(TEST, MSG, OUTERROR) \
        ((TEST) || (c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR(MSG), OUTERROR), false))

    // Calls the function, returning its return value. If an exception is thrown it catches it,
    // stores the error into `outError`, and returns a default 0/nullptr/false value.
    template <typename RESULT, typename LAMBDA>
    ALWAYS_INLINE RESULT tryCatch(C4Error *outError, LAMBDA fn) noexcept {
        try {
            return fn();
        } catchError(outError);
        return RESULT(); // this will be 0, nullptr, false, etc.
    }

    // Calls the function and returns true.
    // If an exception is thrown it catches it, stores the error into `outError`, and returns false.
    template <typename LAMBDA>
    ALWAYS_INLINE bool tryCatch(C4Error *error, LAMBDA fn) noexcept {
        try {
            fn();
            return true;
        } catchError(error);
        return false;
    }

#ifndef c4error_descriptionStr
    // Convenient shortcut for logging the description of a C4Error. Returns a C string pointer.
    // WARNING: The string pointer becomes invalid as soon as the expression outside this call
    // (typically a LiteCore logging function) returns. Don't store it!
    #define c4error_descriptionStr(ERR)     fleece::alloc_slice(c4error_getDescription(ERR)).asString().c_str()
#endif

}
