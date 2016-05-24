//
//  Error.hh
//  CBForest
//
//  Created by Jens Alfke on 6/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef CBForest_Error_h
#define CBForest_Error_h

#include "forestdb.h"
#include <exception>

#undef check

namespace cbforest {

#ifdef _MSC_VER
#define expected(EXPR, VALUE)   (EXPR)
#else
#define expected __builtin_expect
#endif

    /** Most API calls can throw this. */
    struct error : public std::exception {
        // Extra status codes not defined by fdb_errors.h
        enum CBForestError {
            BadRevisionID = -1000,
            CorruptRevisionData = -1001,
            CorruptIndexData = -1002,
            AssertionFailed = -1003,
            TokenizerError = -1004, // can't create tokenizer
            BadVersionVector = -1005
        };

        /** Either an fdb_status code, as defined in fdb_errors.h; or a CBForestError. */
        int const status;

        error (fdb_status s)        :status(s) {}
        error (CBForestError e)     :status(e) {}

        virtual const char *what() const noexcept;

        [[noreturn]] static void _throw(fdb_status);

        [[noreturn]] static void assertionFailed(const char *func, const char *file, unsigned line,
                                                 const char *expr);
    };

    static inline void check(fdb_status status) {
        if (expected(status != FDB_RESULT_SUCCESS, false))
            error::_throw(status);
    }


// Like C assert() but throws an exception instead of aborting
#define	CBFAssert(e) \
    (expected(!(e), 0) ? cbforest::error::assertionFailed(__func__, __FILE__, __LINE__, #e) \
                               : (void)0)

// CBFDebugAssert is removed from release builds; use when 'e' test is too expensive
#ifdef NDEBUG
#define CBFDebugAssert(e)   do{ }while(0)
#else
#define CBFDebugAssert(e)   CBFAssert(e)
#endif

}

#endif
