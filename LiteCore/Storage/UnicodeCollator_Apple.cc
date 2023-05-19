//
// UnicodeCollator_Apple.cc
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
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/Exception.h"
#include <sqlite3.h>

#if __APPLE__  // For Apple platforms; see UnicodeCollator_*.cc for other implementations

#    include <CoreFoundation/CFString.h>

namespace litecore {

    using namespace std;
    using namespace fleece;

    // RAII wrapper for a temporary CFString. It must not outlive the slice it came from.
    class TempCFString {
      public:
        explicit TempCFString(slice str) noexcept
            : _cfStr(CFStringCreateWithBytesNoCopy(nullptr, (const UInt8*)str.buf, static_cast<CFIndex>(str.size),
                                                   kCFStringEncodingUTF8, false, kCFAllocatorNull)) {}

        ~TempCFString() noexcept {
            if ( _cfStr ) CFRelease(_cfStr);
        }

        operator CFStringRef() const noexcept { return _cfStr; }

        TempCFString(const TempCFString&)            = delete;
        TempCFString& operator=(const TempCFString&) = delete;

      private:
        CFStringRef const _cfStr;
    };

    // Stores CF collation parameters for fast lookup; callback context points to this
    class CFCollationContext : public CollationContext {
      public:
        CFLocaleRef          localeRef{nullptr};
        CFStringCompareFlags flags;

        explicit CFCollationContext(const Collation& coll)
            : CollationContext(coll), flags(kCFCompareNonliteral | kCFCompareWidthInsensitive) {
            Assert(coll.unicodeAware);
            if ( !coll.caseSensitive ) flags |= kCFCompareCaseInsensitive;

            if ( !coll.diacriticSensitive ) flags |= kCFCompareDiacriticInsensitive;

            slice localeName = coll.localeName ?: "en_US"_sl;
            flags |= kCFCompareLocalized;
            TempCFString localeStr(localeName);
            if ( localeStr ) localeRef = CFLocaleCreate(nullptr, localeStr);
            if ( !localeRef ) Warn("Unknown locale name '%.*s'", SPLAT(coll.localeName));
        }

        ~CFCollationContext() override {
            if ( localeRef ) CFRelease(localeRef);
        }

        CFCollationContext(const CFCollationContext&)            = delete;
        CFCollationContext& operator=(const CFCollationContext&) = delete;
    };

    unique_ptr<CollationContext> CollationContext::create(const Collation& coll) {
        return make_unique<CFCollationContext>(coll);
    }

    /** Full Unicode-savvy string comparison. */
    __hot static inline int compareStringsUnicode(int len1, const void* chars1, int len2, const void* chars2,
                                                  const CFCollationContext& ctx) {
        // OPT: Consider using UCCompareText(), from <CarbonCore/UnicodeUtilities.h>, instead?

        TempCFString cfstr1({chars1, size_t(len1)});
        TempCFString cfstr2({chars2, size_t(len2)});
        if ( _usuallyFalse(!cfstr1) ) return -1;
        if ( _usuallyFalse(!cfstr2) ) return 1;
        return (int)CFStringCompareWithOptionsAndLocale(cfstr1, cfstr2, CFRange{0, CFStringGetLength(cfstr1)},
                                                        ctx.flags, ctx.localeRef);
    }

    __hot static int collateUnicodeCallback(void* context, int len1, const void* chars1, int len2, const void* chars2) {
        auto& coll = *(CFCollationContext*)context;
        if ( coll.canCompareASCII ) {
            int result = CompareASCII(len1, (const uint8_t*)chars1, len2, (const uint8_t*)chars2, coll.caseSensitive);
            if ( result != kCompareASCIIGaveUp ) return result;
        }
        return compareStringsUnicode(len1, chars1, len2, chars2, coll);
    }

    int CompareUTF8(slice str1, slice str2, const Collation& coll) {
        return CompareUTF8(str1, str2, CFCollationContext(coll));
    }

    int CompareUTF8(slice str1, slice str2, const CollationContext& ctx) {
        return collateUnicodeCallback((void*)&ctx, (int)str1.size, str1.buf, (int)str2.size, str2.buf);
    }

    int LikeUTF8(fleece::slice str1, fleece::slice str2, const Collation& coll) {
        return LikeUTF8(str1, str2, CFCollationContext(coll));
    }

    __hot bool ContainsUTF8(fleece::slice str, fleece::slice substr, const CollationContext& ctx) {
        TempCFString cfStr(str), cfSubstr(substr);
        if ( _usuallyFalse(!cfStr || !cfSubstr) ) return false;
        auto& cfCtx = (const CFCollationContext&)ctx;
        return CFStringFindWithOptionsAndLocale(cfStr, cfSubstr, {0, CFStringGetLength(cfStr)}, cfCtx.flags,
                                                cfCtx.localeRef, nullptr);
    }

    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle, const Collation& coll) {
        unique_ptr<CollationContext> context(new CFCollationContext(coll));
        int rc = sqlite3_create_collation(dbHandle, coll.sqliteName().c_str(), SQLITE_UTF8, (void*)context.get(),
                                          collateUnicodeCallback);
        if ( rc != SQLITE_OK ) throw SQLite::Exception(dbHandle, rc);
        return context;
    }

    vector<string> SupportedLocales() {
        vector<string> locales;
        CFArrayRef     rawLocales = CFLocaleCopyAvailableLocaleIdentifiers();
        auto           count      = CFArrayGetCount(rawLocales);
        char           buf[32];
        for ( CFIndex i = 0; i < count; i++ ) {
            auto locale = (CFStringRef)CFArrayGetValueAtIndex(rawLocales, i);
            CFStringGetCString(locale, buf, 32, kCFStringEncodingASCII);
            locales.emplace_back(buf);
        }

        CFRelease(rawLocales);
        return locales;
    }

}  // namespace litecore

#endif  // __APPLE__
