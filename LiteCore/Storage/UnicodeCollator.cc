//
//  UnicodeCollator.cc
//  LiteCore
//
//  Created by Jens Alfke on 7/28/17.
//Copyright © 2017 Couchbase. All rights reserved.
//

#include "UnicodeCollator.hh"
#include "PlatformCompat.hh"
#include <sqlite3.h>
#include <algorithm>

namespace litecore {

    void RegisterSQLiteUnicodeCollations(sqlite3* dbHandle) {
        sqlite3_collation_needed(dbHandle, nullptr,
                                 [](void *, sqlite3 *db, int textRep, const char *name)
        {
            // Callback from SQLite when it needs a collation:
            int flags;
            if (sscanf(name, "LCUnicode_%d", &flags) > 0) {
                RegisterSQLiteUnicodeCollation(db, name, flags);
            }
        });
    }

    std::string NameOfSQLiteCollation(CollationFlags flags) {
        if (flags & (kUnicodeAware | kDiacriticInsensitive | kLocalized)) {
            char name[20];
            sprintf(name, "LCUnicode_%d", (int)flags | kUnicodeAware);
            return name;
        } else if (flags & kCaseInsensitive) {
            return "NOCASE";
        } else {
            return "BINARY";
        }
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
         23, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73, 75,
         77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 19, 26, 20,  6,  7,
          5, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76,
         78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98, 21, 34, 22, 35,128};

    // Same thing but case-insensitive.
    static const uint8_t kCharPriorityCaseInsensitive[128] = {
         99,100,101,102,103,104,105,106,107,  1,  2,108,109,  3,110,111,
        112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
          4, 12, 16, 28, 36, 29, 27, 15, 17, 18, 24, 30,  9,  8, 14, 25,
         37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 11, 10, 31, 32, 33, 13,
         23, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73, 75,
         77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 19, 26, 20,  6,  7,
          5, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73, 75,
         77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 21, 34, 22, 35,128};

#if 0
    // This function outputs the tables above. It only needs to be run if we change the definition
    // of the tables and need to regenerate them.
    static void generateCharPriorityMap() {
        uint8_t charPriority[128] = {};
        static const char* const kInverseMap = "\t\n\r `^_-,;:!?.'\"()[]{}@*/\\&#%+<=>|~$"
                                "0123456789AaBbCcDdEeFfGgHhIiJjKkLlMmNnOoPpQqRrSsTtUuVvWwXxYyZz";
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

        // Give lowercase letters the same priority as uppercase:
        for (uint8_t c = 'a'; c <= 'z'; c++)
            charPriority[c] = charPriority[toupper(c)];

        printf("static const uint8_t kCharPriorityCaseInsensitive[128] = {");
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


    int CompareASCII(int len1, const void * chars1,
                     int len2, const void * chars2,
                     bool caseInsensitive)
    {
        // Optimistically do an ASCII-only comparison (using Unicode character ordering),
        // but watch out for non-ASCII bytes:
        auto cp1 = (const uint8_t*)chars1, cp2 = (const uint8_t*)chars2;
        for (size_t n = std::min(len1, len2); n > 0; --n) {
            uint8_t c1 = *cp1, c2 = *cp2;
            if (_usuallyFalse((c1 & 0x80) || (c2 & 0x80)))
                return kCompareASCIIGaveUp;
            if (c1 != c2) {
                if (caseInsensitive) {
                    int s = cmp(kCharPriorityCaseInsensitive[c1], kCharPriorityCaseInsensitive[c2]);
                    if (_usuallyTrue(s != 0))
                        return s;
                } else {
                    return cmp(kCharPriority[c1], kCharPriority[c2]);
                }
            }

            cp1++;
            cp2++;
        }

        // One string has ended. The longer string wins; if both are same length, they're equal:
        return cmp(len1, len2);
    }

}
