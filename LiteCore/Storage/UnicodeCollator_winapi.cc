//
// UnicodeCollator_winapi.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "UnicodeCollator.hh"
#include "fleece/PlatformCompat.hh"
#include "Error.hh"
#include "Logging.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "StringUtil.hh"
#include "TempArray.hh"
#include "NumConversion.hh"
#include <sqlite3.h>
#include <algorithm>

#ifdef _MSC_VER

#    include <windows.h>

namespace litecore {

    using namespace std;
    using namespace fleece;

    // Stores Windows collation parameters for fast lookup; callback context points to this
    class WinApiCollationContext : public CollationContext {
      public:
        LPWSTR localeName{nullptr};
        DWORD  flags;

        WinApiCollationContext(const Collation& coll) : CollationContext(coll), flags(NORM_IGNOREWIDTH) {
            Assert(coll.unicodeAware);
            if ( !coll.caseSensitive ) flags |= LINGUISTIC_IGNORECASE;

            if ( !coll.diacriticSensitive ) flags |= LINGUISTIC_IGNOREDIACRITIC;

            slice localeSlice = coll.localeName;
            localeName        = (LPWSTR)calloc(LOCALE_NAME_MAX_LENGTH + 1, sizeof(WCHAR));
            if ( localeSlice.buf == nullptr ) {
                LCID lcid = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);
                LCIDToLocaleName(lcid, localeName, LOCALE_NAME_MAX_LENGTH, 0);
            } else {
                string tmp((const char*)localeSlice.buf, localeSlice.size);
                replace(tmp, '_', '-');
                MultiByteToWideChar(CP_UTF8, 0, tmp.c_str(), narrow_cast<DWORD>(tmp.size()), localeName,
                                    LOCALE_NAME_MAX_LENGTH);
                if ( LocaleNameToLCID(localeName, 0) == 0 ) {
                    Warn("Unknown locale name '%.*s', using default", SPLAT(coll.localeName));
                    LCID lcid = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);
                    LCIDToLocaleName(lcid, localeName, LOCALE_NAME_MAX_LENGTH, 0);
                }
            }
        }

        ~WinApiCollationContext() {
            if ( localeName ) free(localeName);
        }
    };

    unique_ptr<CollationContext> CollationContext::create(const Collation& coll) {
        return make_unique<WinApiCollationContext>(coll);
    }

    /** Full Unicode-savvy string comparison. */
    static inline int compareStringsUnicode(int len1, const void* chars1, int len2, const void* chars2,
                                            const WinApiCollationContext& ctx) {
        LPWSTR locale   = ctx.localeName;
        DWORD  winFlags = ctx.flags;

        // +1 these just in case they end up being the same length to not overrun the buffer
        TempArray(wchars1, WCHAR, len1 + 1);
        int size1 = MultiByteToWideChar(CP_UTF8, 0, (char*)chars1, len1, wchars1, len1 + 1);
        while ( size1 < len1 + 1 ) { wchars1[size1++] = 0; }

        TempArray(wchars2, WCHAR, len2 + 1);
        int size2 = MultiByteToWideChar(CP_UTF8, 0, (char*)chars2, len2, wchars2, len2 + 1);
        while ( size2 < len2 + 1 ) { wchars2[size2++] = 0; }

        int result = CompareStringEx(locale, winFlags, wchars1, -1, wchars2, -1, nullptr, nullptr, 0);
        if ( result == 0 ) {
            DWORD err = GetLastError();
            Warn("Failed to compare strings (Error %lu), arbitrarily returning equal", err);
            return 0;
        }

        if ( result == CSTR_LESS_THAN ) { return -1; }

        if ( result == CSTR_GREATER_THAN ) { return 1; }

        return 0;
    }

    static int collateUnicodeCallback(void* context, int len1, const void* chars1, int len2, const void* chars2) {
        auto& coll = *(WinApiCollationContext*)context;
        if ( coll.canCompareASCII ) {
            int result = CompareASCII(len1, (const uint8_t*)chars1, len2, (const uint8_t*)chars2, coll.caseSensitive);
            if ( result != kCompareASCIIGaveUp ) return result;
        }
        return compareStringsUnicode(len1, chars1, len2, chars2, coll);
    }

    int CompareUTF8(slice str1, slice str2, const Collation& coll) {
        return CompareUTF8(str1, str2, WinApiCollationContext(coll));
    }

    int CompareUTF8(slice str1, slice str2, const CollationContext& ctx) {
        return collateUnicodeCallback((void*)&ctx, (int)str1.size, str1.buf, (int)str2.size, str2.buf);
    }

    int LikeUTF8(fleece::slice str1, fleece::slice str2, const Collation& coll) {
        return LikeUTF8(str1, str2, WinApiCollationContext(coll));
    }

    bool ContainsUTF8(fleece::slice str, fleece::slice substr, const CollationContext& ctx) {
        // FIXME: This is quite slow! Call Windows API instead
        return ContainsUTF8_Slow(str, substr, ctx);
    }

    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle, const Collation& coll) {
        unique_ptr<CollationContext> context(new WinApiCollationContext(coll));
        int rc = sqlite3_create_collation(dbHandle, coll.sqliteName().c_str(), SQLITE_UTF8, (void*)context.get(),
                                          collateUnicodeCallback);
        if ( rc != SQLITE_OK ) throw SQLite::Exception(dbHandle, rc);
        return context;
    }

    BOOL __stdcall SupportedLocalesCallback(LPWSTR name, DWORD flags, LPARAM arg) {
        auto*  locales = (vector<string>*)arg;
        size_t len     = wcslen(name);
        TempArray(buf, char, len + 1);
        buf[len] = 0;
        WideCharToMultiByte(CP_UTF8, 0, name, wcslen(name), buf, (int)len, NULL, NULL);
        locales->push_back(buf);
        return TRUE;
    }

    vector<string> SupportedLocales() {
        vector<string> locales;
        EnumSystemLocalesEx(SupportedLocalesCallback, LOCALE_ALL, (LPARAM)&locales, NULL);
        return locales;
    }
}  // namespace litecore

#endif
