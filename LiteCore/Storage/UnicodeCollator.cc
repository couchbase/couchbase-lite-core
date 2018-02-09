//
// UnicodeCollator.cc
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

#include "UnicodeCollator.hh"
#include "Logging.hh"
#include "PlatformCompat.hh"
#include "StringUtil.hh"
#include <sqlite3.h>
#include <algorithm>

namespace litecore {

    using namespace std;
    using namespace fleece;
    

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

    std::string Collation::sqliteName() const {
        if (unicodeAware) {
            char name[20];
            sprintf(name, "LCUnicode_%c%c_%.*s",
                    caseSensitive ? '_' : 'C',
                    diacriticSensitive ? '_' : 'D',
                    SPLAT(localeName));
            return name;
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
