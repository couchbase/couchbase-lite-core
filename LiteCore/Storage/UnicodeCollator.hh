//
//  UnicodeCollator.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/27/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include <slice.hh>
#include <string>

struct sqlite3;

namespace litecore {

    typedef int CollationFlags;
    enum {
        kUnicodeAware           = 1,
        kCaseInsensitive        = 2,    // Ignore uppercase/lowercase distinction
        kDiacriticInsensitive   = 4,    // Ignore diacritical/accent marks
        kLocalized              = 8     // Use current locale's special sorting rules
    };

    // TODO: Add language/locale specifier to options

    /** Unicode-aware comparison of two UTF8-encoded strings. */
    int CompareUTF8(fleece::slice str1, fleece::slice str2, CollationFlags =0);

    /** Registers a specific SQLite collation function with the given name & flags. */
    int RegisterSQLiteUnicodeCollation(sqlite3*, const char *name, CollationFlags);

    /** Registers all collation functions; actually it registers a callback that lets SQLite ask
        for a specific collation, and then calls RegisterSQLiteUnicodeCollation. */
    void RegisterSQLiteUnicodeCollations(sqlite3*);

    /** Returns the standard name of the collation with the given flags. */
    std::string NameOfSQLiteCollation(CollationFlags);


    /** Simple comparison of two UTF8-encoded strings. Uses Unicode ordering, but gives up
        and returns kCompareASCIIGaveUp if it finds any non-ASCII characters.
        This is used as a subroutine by CompareUTF8(), which is what you should probably call
        instead. */
    int CompareASCII(int len1, const void * chars1,
                     int len2, const void * chars2,
                     bool caseInsensitive);

    /** The value CompareASCII returns if it finds non-ASCII characters in either string. */
    static constexpr int kCompareASCIIGaveUp = 2;

}
