//
// UnicodeCollator.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace litecore {

    enum { kLikeMatch, kLikeNoMatch, kLikeNoWildcardMatch };

    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema#collation
    struct Collation {
        bool                unicodeAware{false};
        bool                caseSensitive{true};
        bool                diacriticSensitive{true};
        fleece::alloc_slice localeName;

        Collation() = default;

        explicit Collation(bool cs, bool ds = true) {
            caseSensitive      = cs;
            diacriticSensitive = ds;
        }

        Collation(bool cs, bool ds, fleece::slice loc) : Collation(cs, ds) {
            unicodeAware = true;
            localeName   = loc;
        }

        /** Returns the name of the SQLite collator with these options. */
        [[nodiscard]] std::string sqliteName() const;

        bool readSQLiteName(const char* name);
    };

    /** Base class of context info managed by collation implementations. */
    class CollationContext {
      public:
        // factory function
        static std::unique_ptr<CollationContext> create(const Collation&);

        virtual ~CollationContext() = default;

        bool canCompareASCII;
        bool caseSensitive;

      protected:
        explicit CollationContext(const Collation& collation)
            : canCompareASCII(true), caseSensitive(collation.caseSensitive) {
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
    bool ContainsUTF8(fleece::slice str, fleece::slice substr, const CollationContext& ctx);

    /** Registers a specific SQLite collation function with the given options.
        The returned object needs to be kept alive until the database is closed, then deleted. */
    std::unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3*, const Collation&);

    /** Registers all collation functions; actually it registers a callback that lets SQLite ask
        for a specific collation, and then calls RegisterSQLiteUnicodeCollation.
        The contexts created by the collations will be added to the vector. */
    void RegisterSQLiteUnicodeCollations(sqlite3*, CollationContextVector&);


    /** Simple comparison of two UTF8- or UTF16-encoded strings. Uses Unicode ordering, but gives
        up and returns kCompareASCIIGaveUp if it finds any non-ASCII characters. */
    template <class CHAR>  // uint8_t or uchar16_t
    int                 CompareASCII(int len1, const CHAR* chars1, int len2, const CHAR* chars2, bool caseSensitive);
    extern template int CompareASCII(int len1, const uint8_t* chars1, int len2, const uint8_t* chars2,
                                     bool caseSensitive);
    extern template int CompareASCII(int len1, const char16_t* chars1, int len2, const char16_t* chars2,
                                     bool caseSensitive);

    /** The value CompareASCII returns if it finds non-ASCII characters in either string. */
    static constexpr int kCompareASCIIGaveUp = 2;

    // for platform implementors only (default implementation of ContainsUTF8)
    bool ContainsUTF8_Slow(fleece::slice str, fleece::slice substr, const CollationContext& ctx);

    std::vector<std::string> SupportedLocales();
}  // namespace litecore
