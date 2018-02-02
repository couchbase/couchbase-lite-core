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
#include "c4Private.h"
#include "Base.hh"
#include "Error.hh"
#include "PlatformCompat.hh"
#include "function_ref.hh"
#include <exception>
#include <functional>

namespace c4Internal {

    // Utilities for handling C++ exceptions and mapping them to C4Errors.
    //
    // These can't be exported from the LiteCore dynamic library because they use C++ classes and
    // calling conventions, so they have to be statically linked into other libraries that use them,
    // like LiteCoreREST and the helper tools.

    /** Sets up a C4Error from a C++ exception. */
    void recordException(const std::exception &e, C4Error* outError) noexcept;

    /** Clears a C4Error back to empty. */
    static inline void clearError(C4Error* outError) noexcept {if (outError) outError->code = 0;}

    /** Macro to substitute for a regular 'catch' block, that saves any exception in OUTERR. */
    #define catchError(OUTERR) \
        catch (const std::exception &x) { \
            c4Internal::recordException(x, OUTERR); \
        }

    #define catchExceptions() \
        catch (const std::exception &) { }

    #define checkParam(TEST, MSG, OUTERROR) \
        ((TEST) || (c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR(MSG), OUTERROR), false))

    // Calls the function, returning its return value. If an exception is thrown, stores the error
    // into `outError`, and returns a default 0/nullptr/false value.
    template <typename RESULT>
    NOINLINE RESULT tryCatch(C4Error *outError, litecore::function_ref<RESULT()> fn) noexcept {
        try {
            return fn();
        } catchError(outError);
        return RESULT(); // this will be 0, nullptr, false, etc.
    }

    // Calls the function and returns true. If an exception is thrown, stores the error
    // into `outError`, and returns false.
    NOINLINE bool tryCatch(C4Error *error, litecore::function_ref<void()> fn) noexcept;

}
