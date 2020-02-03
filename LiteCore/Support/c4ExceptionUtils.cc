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
#include <exception>

using namespace std;
using namespace litecore;
using namespace fleece;

namespace c4Internal {

    __cold
    void recordException(const exception &e, C4Error* outError) noexcept {
        error err = error::convertException(e).standardized();
        c4error_return((C4ErrorDomain)err.domain, err.code, c4str(e.what()), outError);
    }

    __cold
    void recordException(C4Error* outError) noexcept {
        // This rigamarole recovers the current exception being thrown...
        auto xp = std::current_exception();
        if (xp) {
            try {
                std::rethrow_exception(xp);
            } catch(const std::exception& x) {
                // Now we have the exception, so we can record it in outError:
                recordException(x, outError);
                return;
            } catch (...) { }
        }
        c4error_return(LiteCoreDomain, kC4ErrorUnexpectedError,
                       "Unknown C++ exception"_sl, outError);
    }

}
