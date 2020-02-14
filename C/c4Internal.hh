//
// c4Internal.hh
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#include "Base.hh"
#include "Error.hh"
#include "RefCounted.hh"
#include "PlatformCompat.hh"
#include "fleece/Fleece.h"
#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include "function_ref.hh"
#include <functional>

using namespace std;
using namespace litecore;


#define LOCK(MUTEX)     unique_lock<mutex> _lock(MUTEX)
#define UNLOCK()        _lock.unlock();


namespace c4Internal {

    // ERRORS & EXCEPTIONS:

    const size_t kMaxErrorMessagesToSave = 10;

    void recordError(C4ErrorDomain, int code, std::string message, C4Error* outError) noexcept;
    void recordError(C4ErrorDomain, int code, C4Error* outError) noexcept;
    
    // SLICES:

    C4SliceResult sliceResult(const char *str);
    C4SliceResult sliceResult(const string&);

    string toString(C4Slice);

    void destructExtraInfo(C4ExtraInfo&) noexcept;
}

using namespace c4Internal;
