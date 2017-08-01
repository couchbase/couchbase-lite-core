//
//  UnicodeCollator_std.cc
//  LiteCore
//
//  Created by Jens Alfke on 7/31/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "UnicodeCollator.hh"
#include "PlatformCompat.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>

#include <string>
#include <codecvt>
#include <locale>
#include <iostream>

#if !__APPLE__ // See UnicodeCollator_Apple.cc for Mac/iOS/etc implementation

namespace litecore {

    using namespace std;
    using namespace fleece;

    /** Full Unicode-savvy string comparison. */
    static inline int compareStringsUnicode(int len1, const void * chars1,
                                            int len2, const void * chars2,
                                            CollationFlags flags)
    {
        /* This function isn't working correctly yet:
           - No matter what locale string I use, accented letters always sort after unaccented
             letters.
           - If I use the default/current locale -- locale("") -- the wstring_convert calls fail
             apparently because the locale isn't Unicode-aware. But I don't know another way to
             get the current system locale.
           Jens 8/1/17 */
        auto locale = std::locale("en_US.UTF-8"); // i.e. Unicode-aware but non-localized

        wstring str1 = wstring_convert<codecvt_utf8<wchar_t>, wchar_t>{}
                            .from_bytes((const char*)chars1, (const char*)chars1 + len1);
        wstring str2 = wstring_convert<codecvt_utf8<wchar_t>, wchar_t>{}
                            .from_bytes((const char*)chars2, (const char*)chars2 + len2);

        if (flags & kCaseInsensitive) {
            for (auto &c : str1)
                c = tolower(c, locale);
            for (auto &c : str2)
                c = tolower(c, locale);
        }

        auto& collator = std::use_facet<std::collate<wchar_t>>(locale);
        return collator.compare(&str1.front(), &str1.back() + 1,
                                &str2.front(), &str2.back() + 1);
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

#endif // !__APPLE__
