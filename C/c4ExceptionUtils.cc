//
//  c4ExceptionUtils.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/24/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4ExceptionUtils.hh"
#include "c4Private.h"

using namespace std;
using namespace litecore;

namespace c4Internal {

    void recordException(const exception &e, C4Error* outError) noexcept {
        error err = error::convertException(e).standardized();
        c4error_return((C4ErrorDomain)err.domain, err.code, c4str(e.what()), outError);
    }


    bool tryCatch(C4Error *error, function_ref<void()> fn) noexcept {
        try {
            fn();
            return true;
        } catchError(error);
        return false;
    }

}
