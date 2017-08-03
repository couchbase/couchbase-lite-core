//
//  UnicodeCollator_winapi.cc
//  LiteCore
//
//  Created by Jim Borden on 8/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "UnicodeCollator.hh"
#include "PlatformCompat.hh"
#include "Error.hh"
#include "Logging.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "StringUtil.hh"
#include <sqlite3.h>

#ifdef _MSC_VER

#include <windows.h>

namespace litecore {

    using namespace std;
    using namespace fleece;

    // Stores CF collation parameters for fast lookup; callback context points to this
    class WinApiCollationContext : public CollationContext {
    public:
        LPWSTR localeName{ nullptr };
        DWORD flags;

        WinApiCollationContext(const Collation &coll)
            :CollationContext(coll)
            , flags(NORM_IGNOREWIDTH)
        {
            Assert(coll.unicodeAware);
            if (!coll.caseSensitive)
                flags |= LINGUISTIC_IGNORECASE;

            if (!coll.diacriticSensitive)
                flags |= LINGUISTIC_IGNOREDIACRITIC;

            slice localeSlice = coll.localeName;
            localeName = (LPWSTR)calloc(LOCALE_NAME_MAX_LENGTH + 1, sizeof(WCHAR));
            if (localeSlice.buf == nullptr) {
                LCID lcid = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);
                LCIDToLocaleName(lcid, localeName, LOCALE_NAME_MAX_LENGTH, 0);
            }
            else {
                MultiByteToWideChar(CP_UTF8, 0, (char *)localeSlice.buf, localeSlice.size, localeName, LOCALE_NAME_MAX_LENGTH);
                if (LocaleNameToLCID(localeName, 0) == 0) {
                    Warn("Unknown locale name '%.*s', using default", SPLAT(coll.localeName));
                    LCID lcid = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);
                    LCIDToLocaleName(lcid, localeName, LOCALE_NAME_MAX_LENGTH, 0);
                }
            }
        }

        ~WinApiCollationContext() {
            if (localeName)
                free(localeName);
        }
    };


    /** Full Unicode-savvy string comparison. */
    static inline int compareStringsUnicode(int len1, const void * chars1,
        int len2, const void * chars2,
        const WinApiCollationContext &ctx)
    {
        LPWSTR locale = ctx.localeName;
        DWORD winFlags = ctx.flags;

        StackArray(wchars1, WCHAR, len1);
        int size1 = MultiByteToWideChar(CP_UTF8, 0, (char *)chars1, len1, wchars1, len1);
        wchars1[size1++] = 0;

        StackArray(wchars2, WCHAR, len2);
        int size2 = MultiByteToWideChar(CP_UTF8, 0, (char *)chars2, len2, wchars2, len2);
        wchars2[size2++] = 0;

        int result = CompareStringEx(locale, winFlags, wchars1, size1, wchars2, size2, nullptr, nullptr, 0);
        if (result == 0) {
            DWORD err = GetLastError();
            Warn("Failed to compare strings (Error %d), arbitrarily returning equal", err);
            return 0;
        }

        if (result == CSTR_LESS_THAN) {
            return -1;
        }

        if (result == CSTR_GREATER_THAN) {
            return 1;
        }

        return 0;
    }


    static int collateUnicodeCallback(void *context,
        int len1, const void * chars1,
        int len2, const void * chars2)
    {
        auto &coll = *(WinApiCollationContext*)context;
        if (coll.canCompareASCII) {
            int result = CompareASCII(len1, (const uint8_t*)chars1, len2, (const uint8_t*)chars2,
                coll.caseSensitive);
            if (result != kCompareASCIIGaveUp)
                return result;
        }
        return compareStringsUnicode(len1, chars1, len2, chars2, coll);
    }


    int CompareUTF8(slice str1, slice str2, const Collation &coll) {
        WinApiCollationContext ctx(coll);
        return collateUnicodeCallback(&ctx, (int)str1.size, str1.buf,
            (int)str2.size, str2.buf);
    }


    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle,
        const Collation &coll) {
        unique_ptr<CollationContext> context(new WinApiCollationContext(coll));
        int rc = sqlite3_create_collation(dbHandle,
            coll.sqliteName().c_str(),
            SQLITE_UTF8,
            (void*)context.get(),
            collateUnicodeCallback);
        if (rc != SQLITE_OK)
            throw SQLite::Exception(dbHandle, rc);
        return context;
    }
}

#endif