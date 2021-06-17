//
// UnicodeCollator.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "fleece/slice.hh"
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace litecore {

    enum {
        kLikeMatch,
        kLikeNoMatch,
        kLikeNoWildcardMatch
    };

    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema#collation
    struct Collation {
        bool unicodeAware {false};
        bool caseSensitive {true};
        bool diacriticSensitive {true};
        fleece::alloc_slice localeName;

        Collation() =default;

        explicit Collation(bool cs, bool ds =true) {
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


    /** Base class of context info managed by collation implementations. */
    class CollationContext {
    public:
        // factory function
        static std::unique_ptr<CollationContext> create(const Collation&);

        virtual ~CollationContext() =default;

        bool canCompareASCII;
        bool caseSensitive;

    protected:
        CollationContext(const Collation &collation)
        :caseSensitive(collation.caseSensitive)
        ,canCompareASCII(true)
        {
            //TODO: Some locales have unusual rules for ASCII; for these, clear canCompareASCII.
        }
    };

    using CollationContextVector = std::vector<std::unique_ptr<CollationContext>>;

    /** Unicode-aware comparison of two UTF8-encoded strings. */
    int CompareUTF8(fleece::slice pattern, fleece::slice comparand, const Collation&);
    int CompareUTF8(fleece::slice str1, fleece::slice str2, const CollationContext&);

    /** Unicode-aware LIKE function accepting two UTF-8 encoded strings */
    int LikeUTF8(fleece::slice str1, fleece::slice str2, const Collation&);
    int LikeUTF8(fleece::slice str1, fleece::slice str2, const CollationContext&);

    /** Unicode-aware string containment function accepting two UTF-8 encoded strings*/
    bool ContainsUTF8(fleece::slice str, fleece::slice substr, const CollationContext &ctx);

    /** Registers a specific SQLite collation function with the given options.
        The returned object needs to be kept alive until the database is closed, then deleted. */
    std::unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3*, const Collation&);

    /** Registers all collation functions; actually it registers a callback that lets SQLite ask
        for a specific collation, and then calls RegisterSQLiteUnicodeCollation.
        The contexts created by the collations will be added to the vector. */
    void RegisterSQLiteUnicodeCollations(sqlite3*, CollationContextVector&);


    /** Simple comparison of two UTF8- or UTF16-encoded strings. Uses Unicode ordering, but gives
        up and returns kCompareASCIIGaveUp if it finds any non-ASCII characters. */
    template <class CHAR>       // uint8_t or uchar16_t
    int CompareASCII(int len1, const CHAR *chars1,
                     int len2, const CHAR *chars2,
                     bool caseSensitive);

    /** The value CompareASCII returns if it finds non-ASCII characters in either string. */
    static constexpr int kCompareASCIIGaveUp = 2;

    // for platform implementors only (default implementation of ContainsUTF8)
    bool ContainsUTF8_Slow(fleece::slice str, fleece::slice substr, const CollationContext &ctx);
}
