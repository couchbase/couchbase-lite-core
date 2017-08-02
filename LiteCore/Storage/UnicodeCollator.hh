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

    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema#collation
    struct Collation {
        bool unicodeAware {false};
        bool caseSensitive {true};
        bool diacriticSensitive {true};
        fleece::slice localeName;

        Collation() { }

        Collation(bool cs, bool ds =true) {
            caseSensitive = cs;
            diacriticSensitive = ds;
        }

        Collation(bool cs, bool ds, fleece::slice loc)
        :Collation(cs, ds)
        {
            unicodeAware = true;
            localeName = loc;
        }

        /** Returns the name of the SQLite collator with these options. */
        std::string sqliteName() const;

        bool readSQLiteName(const char *name);
    };


    /** Unicode-aware comparison of two UTF8-encoded strings. */
    int CompareUTF8(fleece::slice str1, fleece::slice str2, const Collation&);

    /** Registers a specific SQLite collation function with the given name & flags. */
    int RegisterSQLiteUnicodeCollation(sqlite3*, const Collation&);

    /** Registers all collation functions; actually it registers a callback that lets SQLite ask
        for a specific collation, and then calls RegisterSQLiteUnicodeCollation. */
    void RegisterSQLiteUnicodeCollations(sqlite3*);


    /** Simple comparison of two UTF8- or UTF16-encoded strings. Uses Unicode ordering, but gives
        up and returns kCompareASCIIGaveUp if it finds any non-ASCII characters. */
    template <class CHAR>       // uint8_t or uchar16_t
    int CompareASCII(int len1, const CHAR *chars1,
                     int len2, const CHAR *chars2,
                     bool caseSensitive);

    /** The value CompareASCII returns if it finds non-ASCII characters in either string. */
    static constexpr int kCompareASCIIGaveUp = 2;

}
