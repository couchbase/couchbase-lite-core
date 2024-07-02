//
// UnicodeCollator_JS.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// This is an UnicodeCollaction implementation based on the JS Intl.Collator API.
// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Collator

#include "UnicodeCollator.hh"
#include "Error.hh"
#include "emscripten/val.h"
#include "SQLiteCpp/Exception.h"
#include <sqlite3.h>

namespace litecore {

    using namespace std;
    using namespace emscripten;
    using namespace fleece;

    class JSCollationContext : public CollationContext {
      public:
        val collator = val::undefined();

        JSCollationContext(const Collation& collation) : CollationContext(collation) {
            auto locale  = val::undefined();
            auto options = val::object();

            if ( collation.localeName ) { locale = val(collation.localeName.asString()); }

            if ( collation.diacriticSensitive ) {
                if ( collation.caseSensitive ) {
                    options.set("sensitivity", "variant");
                } else {
                    options.set("sensitivity", "accent");
                }
            } else {
                if ( collation.caseSensitive ) {
                    options.set("sensitivity", "case");
                } else {
                    options.set("sensitivity", "base");
                }
            }

            collator = val::global("Intl")["Collator"].new_(locale, options);
        }
    };

    unique_ptr<CollationContext> CollationContext::create(const Collation& coll) {
        return make_unique<JSCollationContext>(coll);
    }

    static inline int compareStringsUnicode(int len1, const void* chars1, int len2, const void* chars2,
                                            const JSCollationContext& ctx) {
        return ctx.collator.call<int>("compare", string((const char*)chars1, len1), string((const char*)chars2, len2));
    }

    static int collateUnicodeCallback(void* context, int len1, const void* chars1, int len2, const void* chars2) {
        auto& coll = *(JSCollationContext*)context;
        if ( coll.canCompareASCII ) {
            int result = CompareASCII(len1, (const uint8_t*)chars1, len2, (const uint8_t*)chars2, coll.caseSensitive);
            if ( result != kCompareASCIIGaveUp ) return result;
        }
        return compareStringsUnicode(len1, chars1, len2, chars2, coll);
    }

    int CompareUTF8(slice str1, slice str2, const Collation& coll) {
        return CompareUTF8(str1, str2, JSCollationContext(coll));
    }

    int CompareUTF8(slice str1, slice str2, const CollationContext& ctx) {
        return collateUnicodeCallback((void*)&ctx, (int)str1.size, str1.buf, (int)str2.size, str2.buf);
    }

    int LikeUTF8(slice str1, slice str2, const Collation& coll) {
        return LikeUTF8(str1, str2, JSCollationContext(coll));
    }

    bool ContainsUTF8(slice str, slice substr, const CollationContext& ctx) {
        return ContainsUTF8_Slow(str, substr, ctx);
    }

    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle, const Collation& coll) {
        unique_ptr<CollationContext> context(new JSCollationContext(coll));
        int rc = sqlite3_create_collation(dbHandle, coll.sqliteName().c_str(), SQLITE_UTF8, (void*)context.get(),
                                          collateUnicodeCallback);
        if ( rc != SQLITE_OK ) throw SQLite::Exception(dbHandle, rc);
        return context;
    }

}  // namespace litecore
