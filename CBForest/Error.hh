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

#ifndef __attribute
#define __attribute(x)
#endif

#undef check

namespace forestdb {

    /** Most API calls can throw this. */
    struct error {
        // Extra status codes not defined by fdb_errors.h
        enum CBForestError {
            BadRevisionID = -1000,
            CorruptRevisionData = -1001,
            CorruptIndexData = -1002,
            AssertionFailed = -1003
        };

        /** Either an fdb_status code, as defined in fdb_errors.h; or a CBForestError. */
        int status;

        const char *message() const {return fdb_error_msg((fdb_status)status);}

        error (fdb_status s)        :status(s) {}
        error (CBForestError e)     :status(e) {}

        static void _throw(fdb_status) __attribute((noreturn));

        static void assertionFailed(const char *func, const char *file, unsigned line,
                                    const char *expr)   __attribute((noreturn));
    };

    static inline void check(fdb_status status) {
#ifdef _MSC_VER
        if (status != FDB_RESULT_SUCCESS)
            error::_throw(status);
#else
        if (__builtin_expect(status != FDB_RESULT_SUCCESS, 0))
            error::_throw(status);
#endif
    }


    // Like C assert() but throws an exception instead of aborting
#ifdef _MSC_VER
	#define CBFAssert(e) \
		if(!(e)) forestdb::error::assertionFailed(__FUNCTION__, __FILE__, __LINE__, #e)
#else
    #define	CBFAssert(e) \
        (__builtin_expect(!(e), 0) ? forestdb::error::assertionFailed(__func__, __FILE__, __LINE__, #e) \
                                   : (void)0)
#endif


}

#endif
