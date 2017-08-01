//
//  UnicodeCollator_Apple.cc
//  LiteCore
//
//  Created by Jens Alfke on 7/27/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "UnicodeCollator.hh"
#include "PlatformCompat.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>

#if __APPLE__ // For Apple platforms; see UnicodeCollator_std.cc for cross-platform implementation

#include <CoreFoundation/CFString.h>


namespace litecore {

    using namespace std;
    using namespace fleece;

    /** Full Unicode-savvy string comparison. */
    static inline int compareStringsUnicode(int len1, const void * chars1,
                                            int len2, const void * chars2,
                                            CollationFlags flags)
    {
        // OPT: Consider using UCCompareText(), from <CarbonCore/UnicodeUtilities.h>, instead?
        // The CollatorRef can be created when the callback is registered, and passed in via the
        // callback's 'context' parameter.

        auto cfstr1 = CFStringCreateWithBytesNoCopy(nullptr, (const UInt8*)chars1, len1,
                                                    kCFStringEncodingUTF8, false, kCFAllocatorNull);
        if (_usuallyFalse(!cfstr1))
            return -1;
        auto cfstr2 = CFStringCreateWithBytesNoCopy(nullptr, (const UInt8*)chars2, len2,
                                                    kCFStringEncodingUTF8, false, kCFAllocatorNull);
        if (_usuallyFalse(!cfstr2)) {
            CFRelease(cfstr1);
            return 1;
        }

        auto cfFlags = kCFCompareNonliteral | kCFCompareWidthInsensitive;
        if (flags & kCaseInsensitive)
            cfFlags |= kCFCompareCaseInsensitive;
        if (flags & kDiacriticInsensitive)
            cfFlags |= kCFCompareDiacriticInsensitive;
        if (flags & kLocalized)
            cfFlags |= kCFCompareLocalized;

        int result = CFStringCompare(cfstr1, cfstr2, cfFlags);
        CFRelease(cfstr1);
        CFRelease(cfstr2);
        return result;
    }


    static int collateUnicodeCallback(void *context,
                                      int len1, const void * chars1,
                                      int len2, const void * chars2)
    {
        auto flags = (CollationFlags)(size_t)context;
        int result = CompareASCII(len1, chars1, len2, chars2,
                                  (flags & kCaseInsensitive) != 0);
        if (result == kCompareASCIIGaveUp)
            result = compareStringsUnicode(len1, chars1, len2, chars2, flags);
        return result;
    }


    int CompareUTF8(slice str1, slice str2, CollationFlags flags) {
        auto context = (void*)(size_t)flags;
        return collateUnicodeCallback(context, (int)str1.size, str1.buf,
                                               (int)str2.size, str2.buf);
    }


    int RegisterSQLiteUnicodeCollation(sqlite3* dbHandle, const char *name, CollationFlags flags) {
        auto context = (void*)(size_t)flags;
        return sqlite3_create_collation(dbHandle, name, SQLITE_UTF8,
                                        context, collateUnicodeCallback);
    }
}

#endif // __APPLE__
