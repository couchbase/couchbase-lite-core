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
#include <string>


namespace cbforest {

    const char* error::what() const noexcept {
        return fdb_error_msg((fdb_status)status);
    }

    
    void error::_throw(fdb_status status) {
        WarnError("%s (%d)\n", fdb_error_msg(status), status);
        throw error{status};
    }


    void error::assertionFailed(const char *fn, const char *file, unsigned line, const char *expr) {
        if (LogLevel > kError || LogCallback == NULL)
            fprintf(stderr, "Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        WarnError("Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        throw error(error::AssertionFailed);
    }
    
}
