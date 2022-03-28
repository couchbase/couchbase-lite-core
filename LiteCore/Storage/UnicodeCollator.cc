//
// UnicodeCollator.cc
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
#include "Logging.hh"
#include "fleece/PlatformCompat.hh"
#include "StringUtil.hh"
#include <sqlite3.h>
#include <algorithm>

namespace litecore {

    using namespace std;
    using namespace fleece;

    static inline slice ReadUTF8(slice& str) {
        slice retVal = NextUTF8(str);
        str.moveStart(retVal.size);
        return retVal;
    }

    void RegisterSQLiteUnicodeCollations(sqlite3* dbHandle,
                                         CollationContextVector &contexts) {
        sqlite3_collation_needed(dbHandle, &contexts,
                                 [](void *pContexts, sqlite3 *db, int textRep, const char *name)
        {
            // Callback from SQLite when it needs a collation:
            try {
                Collation coll;
                if (coll.readSQLiteName(name)) {
                    auto ctx = RegisterSQLiteUnicodeCollation(db, coll);
                    if (ctx)
                        (*(CollationContextVector*)pContexts).push_back(move(ctx));
                }
            } catch (std::runtime_error &x) {
                Warn("Exception registering a collator: %s", x.what());
            } catch (...) {
                Warn("Unexpected unknown exception registering a collator");
            }
        });
    }

    __hot
    bool ContainsUTF8_Slow(fleece::slice str, fleece::slice substr, const CollationContext &ctx) {
        auto current = substr;
        while(str.size > 0) {
            size_t nextStrSize = NextUTF8Length(str);
            size_t nextSubstrSize = NextUTF8Length(current);
            if(!CompareUTF8({str.buf, nextStrSize}, {current.buf, nextSubstrSize}, ctx)) {
                // The characters are a match, move to the next substring character
                current.moveStart(nextSubstrSize);
                if(current.size == 0) {
                    // Found a match!
                    return true;
                }
            } else {
                current = substr;
            }

            str.moveStart(nextStrSize);
        }
        return false;
    }


    __hot
    int LikeUTF8(slice comparand, slice pattern, const CollationContext& col) {
        // Based on SQLite's 'patternCompare' function (simplified)
        slice c, c2;                       /* Next pattern and input string chars */
        slice matchOne = "_"_sl;
        slice matchAll = "%"_sl;
        slice zEscaped = nullslice;          /* One past the last escaped input char */
          
        while( (c = ReadUTF8(pattern)).size !=0 ){
            if( c==matchAll ){  /* Match "*" */
              /* Skip over multiple "*" characters in the pattern.  If there
              ** are also "?" characters, skip those as well, but consume a
              ** single character of the input string for each "?" skipped */
                while( (c = ReadUTF8(pattern)) == matchAll || c == matchOne ){
                    if( c == matchOne && ReadUTF8(comparand).size == 0 ){
                      return kLikeNoWildcardMatch;
                    }
                }
                if( c.size == 0 ){
                    return kLikeMatch;   /* "*" at the end of the pattern matches */
                }else if( c == "\\"_sl ){
                    c = ReadUTF8(pattern);
                    if( c.size == 0 ) return kLikeNoWildcardMatch;
                }

                /* At this point variable c contains the first character of the
                ** pattern string past the "*".  Search in the input string for the
                ** first matching character and recursively continue the match from
                ** that point.
                **
                */
                while( (c2 = ReadUTF8(comparand)).size != 0 ){
                    if( CompareUTF8(c2, c, col) ) continue;
                    int bMatch = LikeUTF8(comparand, pattern, col);
                    if( bMatch != kLikeNoMatch ) return bMatch;
                }

                return kLikeNoWildcardMatch;
            }

            if( c == "\\"_sl ) {
                c = ReadUTF8(pattern);
                if( c.size == 0 ) return kLikeNoMatch;
                zEscaped = pattern;
            }
            c2 = ReadUTF8(comparand);
            if( !CompareUTF8(c2, c, col) ) continue;
            if( c == matchOne && pattern.buf != zEscaped.buf && c2.size != 0 ) continue;
            return kLikeNoMatch;
          }
          return comparand.size == 0 ? kLikeMatch : kLikeNoMatch;
    }


    std::string Collation::sqliteName() const {
        if (unicodeAware) {
            std::stringstream name;
            name << "LCUnicode_"
                 << (caseSensitive ? '_' : 'C')
                 << (diacriticSensitive ? '_' : 'D')
                 << '_' << (string)localeName;
            return name.str();
        } else if (caseSensitive) {
            return "BINARY";
        } else {
            return "NOCASE";
        }
    }


    bool Collation::readSQLiteName(const char *name) {
        // This only has to support the Unicode-aware names, since BINARY and NOCASE are built in
        char caseFlag, diacFlag;
        char locale[20] = "";
        auto scanned = sscanf(name, "LCUnicode_%c%c_%19s", &caseFlag, &diacFlag, locale);
        if (scanned < 2)
            return false;
        unicodeAware = true;
        caseSensitive = (caseFlag != 'C');
        diacriticSensitive = (diacFlag != 'D');
        localeName = (scanned >= 3) ? alloc_slice(locale) : nullslice;
        return true;
    }

    // This source file does not implement CompareUTF8() or RegisterSQLiteUnicodeCollation(),
    // which are platform-dependent. Those appear in platform-specific source files.


#pragma mark - ASCII COLLATOR:


    // Maps an ASCII character to its relative priority in the Unicode collation sequence.
    static const uint8_t kCharPriority[128] = {
         99,100,101,102,103,104,105,106,107,  1,  2,108,109,  3,110,111,
        112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
          4, 12, 16, 28, 36, 29, 27, 15, 17, 18, 24, 30,  9,  8, 14, 25,
         37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 11, 10, 31, 32, 33, 13,
         23, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76,
         78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98, 19, 26, 20,  6,  7,
          5, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73, 75,
         77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 21, 34, 22, 35,128};

#if 0
    // This function outputs the table above. It only needs to be run if we change the definition
    // of the table and need to regenerate it.
    static void generateCharPriorityMap() {
        uint8_t charPriority[128] = {};
        static const char* const kInverseMap = "\t\n\r `^_-,;:!?.'\"()[]{}@*/\\&#%+<=>|~$"
                                "0123456789aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";
        uint8_t priority = 1;
        for (unsigned i = 0; i < strlen(kInverseMap); i++)
            charPriority[(uint8_t)kInverseMap[i]] = priority++;
        for (unsigned i = 0; i < 128; i++)
            if (charPriority[i] == 0)
                charPriority[i] = priority++;   // fill the rest of the table

        printf("static const uint8_t kCharPriority[128] = {");
        for (unsigned i = 0; i < 128; ++i) {
            if (i > 0)
                printf(",");
            if (i % 16 == 0)
                printf("\n    ");
            printf("%3u", charPriority[i]);
        }
        printf("};\n");
    }
#endif


    template <typename N>
    static inline int cmp(N n1, N n2) {
        return (n1>n2) ? 1 : ((n1<n2) ? -1 : 0);
    }


    template <class CHAR>
    __hot
    int CompareASCII(int len1, const CHAR *chars1,
                     int len2, const CHAR *chars2,
                     bool caseSensitive)
    {
        int tieBreaker = 0;
        auto cp1 = chars1, cp2 = chars2;
        for (size_t n = std::min(len1, len2); n > 0; --n) {
            auto c1 = *cp1, c2 = *cp2;
            if (_usuallyFalse((c1 >= 0x80) || (c2 >= 0x80)))
                return kCompareASCIIGaveUp;
            auto x = c1 ^ c2;
            if (_usuallyFalse(x != 0)) {
                // Characters are different:
                if (x == 0x20 && tolower(c1) == tolower(c2)) {
                    // Case-equivalent:
                    if (caseSensitive && tieBreaker == 0)
                        tieBreaker = cmp(kCharPriority[c1], kCharPriority[c2]);
                } else {
                    // Not case-equivalent: rank strings by priority of these chars
                    return cmp(kCharPriority[c1], kCharPriority[c2]);
                }
            }

            cp1++;
            cp2++;
        }

        // One string has ended. The longer string wins; if both are same length, they're equal
        // ignoring case; if we're not ignoring case, consider the first different characters.
        int result = cmp(len1, len2);
        return result ? result : tieBreaker;
    }

    // Explicitly instantiate the template for 8- and 16-bit chars:
    template int CompareASCII(int len1, const uint8_t *chars1,
                              int len2, const uint8_t *chars2,
                              bool caseSensitive);
    template int CompareASCII(int len1, const char16_t *chars1,
                              int len2, const char16_t *chars2,
                              bool caseSensitive);

}
