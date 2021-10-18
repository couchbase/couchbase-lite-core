//
// StringUtil_Apple.mm
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
