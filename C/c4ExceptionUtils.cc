//
// c4ExceptionUtils.cc
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
