//
//  StringUtil_Apple.mm
//  LiteCore
//
//  Created by Jens Alfke on 8/8/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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
