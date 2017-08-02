//
//  UnicodeCollator_ICU.cc
//  LiteCore
//
//  Created by Jens Alfke on 7/31/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "UnicodeCollator.hh"
#include "Error.hh"
#include "Logging.hh"
#include "PlatformCompat.hh"
#include "StringUtil.hh"
#include <sqlite3.h>

#include <string>
#include <codecvt>
#include <locale>
#include <iostream>

#if !__APPLE__ && !_MSC_VER // See UnicodeCollator_Apple.cc for Mac/iOS/etc implementation

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
    using namespace litecore;


    struct ICUCollationContext {
        UCollator* ucoll {nullptr};
        bool canCompareASCII;
        bool caseSensitive;

        ICUCollationContext(const Collation &collation)
        :caseSensitive(collation.caseSensitive)
        ,canCompareASCII(true)
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


    /** Full Unicode-savvy string comparison. */
    static inline int compareStringsUnicode(int len1, const void * chars1,
                                            int len2, const void * chars2,
                                            const ICUCollationContext &ctx)
    {
        UErrorCode status = U_ZERO_ERROR;
        int result = ucol_strcollUTF8(ctx.ucoll, (const char*)chars1, len1,
                                                 (const char*)chars2, len2, &status);
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
        ICUCollationContext ctx(coll);
        return collateUnicodeCallback(&ctx, (int)str1.size, str1.buf,
                                      (int)str2.size, str2.buf);
    }


    int RegisterSQLiteUnicodeCollation(sqlite3* dbHandle, const Collation &coll) {
        auto permaColl = new ICUCollationContext(coll);   //TEMP FIXME leak!!
        return sqlite3_create_collation(dbHandle,
                                        coll.sqliteName().c_str(),
                                        SQLITE_UTF8,
                                        permaColl,
                                        collateUnicodeCallback);
    }

}

#endif // !__APPLE__
