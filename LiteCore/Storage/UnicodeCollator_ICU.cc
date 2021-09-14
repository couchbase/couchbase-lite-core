//
// UnicodeCollator_ICU.cc
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
#include "PlatformCompat.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/Exception.h"
#include <sqlite3.h>

#include <string>
#if !defined __ANDROID__ || defined __clang__
#include <codecvt>
#endif
#include <locale>
#include <iostream>

#if LITECORE_USES_ICU // See UnicodeCollator_*.cc for other implementations

/* Note: To build and test this collator in Xcode on a Mac (useful during development) you'll
 need to:
     1. Install ICU ("brew install icu4c")
     2. In LiteCore.xcconfig add /usr/local/opt/icu4c/include at the _start_ of HEADER_SEARCH_PATHS.
     3. In both LiteCore.xcconfig and CppTests.xcconfig, add a line
            OTHER_LDFLAGS = -L/usr/local/opt/icu4c/lib -licui18n  -licuuc
     4. Change "__APPLE__" to "x__APPLE__" in in UnicodeCollator_Apple.cc,
        so this implementation gets used instead of the CFString one.
     5. Define LITECORE_USES_ICU in the prefix header or a xcconfig
 Of course these changes are temporary and shouldn't be committed!
*/

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#include <unicode/uloc.h>
#include <unicode/ucol.h>
#pragma clang diagnostic pop

// http://userguide.icu-project.org/collation
// http://userguide.icu-project.org/collation/api
// http://icu-project.org/apiref/icu4c/ucol_8h.html

namespace litecore {

    using namespace std;
    using namespace fleece;


    class ICUCollationContext : public CollationContext {
    public:
        UCollator* ucoll {nullptr};

        ICUCollationContext(const Collation &collation)
        :CollationContext(collation)
        {
            UErrorCode status = U_ZERO_ERROR;
            ucoll = ucol_open(collation.localeName.asString().c_str(), &status);
            if (U_SUCCESS(status)) {
                if (status == U_USING_DEFAULT_WARNING)
                    Warn("LiteCore indexer: unknown locale '%.*s', using default collator",
                         SPLAT(collation.localeName));

                if (collation.diacriticSensitive) {
                    if (!collation.caseSensitive)
                        ucol_setAttribute(ucoll, UCOL_STRENGTH, UCOL_SECONDARY, &status);
                } else {
                    ucol_setAttribute(ucoll, UCOL_STRENGTH, UCOL_PRIMARY, &status);
                    if (collation.caseSensitive)
                        ucol_setAttribute(ucoll, UCOL_CASE_LEVEL, UCOL_ON, &status);
                }
            }
            if (U_FAILURE(status))
                error::_throw(error::UnexpectedError, "Failed to set up collation (ICU error %d)",
                              (int)status);
        }

        ~ICUCollationContext() {
            if (ucoll)
                ucol_close(ucoll);
        }
    };


    unique_ptr<CollationContext> CollationContext::create(const Collation &coll) {
        return make_unique<ICUCollationContext>(coll);
    }


    /** Full Unicode-savvy string comparison. */
    static inline int compareStringsUnicode(int len1, const void *chars1,
                                            int len2, const void *chars2,
                                            const ICUCollationContext &ctx) {
        UErrorCode status = U_ZERO_ERROR;
#ifdef __ANDROID__
        // Android 4.1 (API 16-17) comes with ICU 4.8 which does not support `ucol_strcollUTF8(...)`
        UCharIterator sIter, tIter;
        uiter_setUTF8(&sIter, (const char *) chars1, len1);
        uiter_setUTF8(&tIter, (const char *) chars2, len2);
        int result = ucol_strcollIter(ctx.ucoll, &sIter, &tIter, &status);
#else
        int result = ucol_strcollUTF8(ctx.ucoll, (const char*)chars1, len1,
                                                 (const char*)chars2, len2, &status);
#endif
        if (U_FAILURE(status))
            Warn("Unicode collation failed with ICU status %d", status);
        return result;
    }


    static int collateUnicodeCallback(void *context,
                                      int len1, const void * chars1,
                                      int len2, const void * chars2)
    {
        auto &coll = *(ICUCollationContext*)context;
        if (coll.canCompareASCII) {
            int result = CompareASCII(len1, (const uint8_t*)chars1,
                                      len2, (const uint8_t*)chars2, coll.caseSensitive);
            if (result != kCompareASCIIGaveUp)
                return result;
        }
        return compareStringsUnicode(len1, chars1, len2, chars2, coll);
    }


    int CompareUTF8(slice str1, slice str2, const Collation &coll) {
        return CompareUTF8(str1, str2, ICUCollationContext(coll));
    }


    int CompareUTF8(slice str1, slice str2, const CollationContext &ctx) {
        return collateUnicodeCallback((void*)&ctx, (int)str1.size, str1.buf,
                                                   (int)str2.size, str2.buf);
    }


    int LikeUTF8(fleece::slice str1, fleece::slice str2, const Collation &coll) {
        return LikeUTF8(str1, str2, ICUCollationContext(coll));
    }


    bool ContainsUTF8(fleece::slice str, fleece::slice substr, const CollationContext &ctx) {
        // FIXME: This is quite slow! Call ICU instead
        return ContainsUTF8_Slow(str, substr, ctx);
    }


    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle,
                                                                const Collation &coll) {
        unique_ptr<CollationContext> context(new ICUCollationContext(coll));
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

#endif // !__APPLE__
