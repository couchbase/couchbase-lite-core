//
// StringUtil_Apple.mm
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

#if __APPLE__
#include "StringUtil.hh"
#include "Logging.hh"

#include <Foundation/Foundation.h>

namespace litecore {
    using namespace fleece;


    static CFMutableStringRef createMutableCFString(slice str) {
        auto cfstr = str.createCFString();
        if (_usuallyFalse(!cfstr))
            return nullptr;
        auto mutstr = CFStringCreateMutableCopy(nullptr, CFStringGetLength(cfstr), cfstr);
        CFRelease(cfstr);
        return mutstr;
    }


    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        auto mutstr = createMutableCFString(str);
        if (_usuallyFalse(!mutstr))
            return {};
        if (toUppercase)
            CFStringUppercase(mutstr, nullptr);     // TODO: provide a locale
        else
            CFStringLowercase(mutstr, nullptr);
        alloc_slice result(mutstr);
        CFRelease(mutstr);
        return result;
    }

}

#endif // __APPLE__
