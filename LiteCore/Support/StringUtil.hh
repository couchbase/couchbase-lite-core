//
// StringUtil.hh
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
#include "PlatformCompat.hh"
#include <stdarg.h>
#include <string>
#include <vector>
#include <sstream>
#if defined __ANDROID__ && !defined __clang__
// gcc for android is not fully c++11 compliant, so we define the missing functions here
namespace std  {
    std::string to_string(const double &n);
    std::string to_string(const long long &n);
    std::string to_string(const unsigned long long &n);
    std::string to_string(const uint16_t &n);
    std::string to_string(const size_t &n);

    double stod(std::string s);
    int stoi(std::string s);
    long long stoll(std::string s);
}
#endif

namespace litecore {

    // Adds EXPR to a stringstream and returns the resulting string.
    // Example: CONCAT("2+2=" << 4 << "!") --> "2+2=4!"
#ifndef __clang__
    #define CONCAT(EXPR)   (static_cast<std::stringstream&>(std::stringstream() << EXPR)).str()
#else
    #define CONCAT(EXPR)   (std::stringstream() << EXPR).str()
#endif

    /** Writes a slice to a stream with the usual "<<" syntax */
    static inline std::ostream& operator<< (std::ostream& o, fleece::slice s) {
        o.write((const char*)s.buf, s.size);
        return o;
    }

    /** Like sprintf(), but returns a std::string */
    std::string format(const char *fmt NONNULL, ...) __printflike(1, 2);

    /** Like vsprintf(), but returns a std::string */
    std::string vformat(const char *fmt NONNULL, va_list);

    /** Returns the strings in the vector concatenated together,
        with the separator (if non-null) between them. */
    std::string join(const std::vector<std::string>&,
                     const char *separator =nullptr);

    /** Concatenates the strings in the vector onto the stream,
        with the separator (if non-null) between them. */
    std::stringstream& join(std::stringstream&,
                            const std::vector<std::string>&,
                            const char *separator =nullptr);

    /** Removes last character from string (in place.) Does nothing if string is empty. */
    void chop(std::string&) noexcept;

    /** Removes last character from string (in place), but only if it equals `ending` */
    void chomp(std::string&, char ending) noexcept;

    /** Replaces all occurrences of `oldChar` with `newChar`. */
    void replace(std::string &str, char oldChar, char newChar);

    /** Replaces all occurrences of `oldStr` with `newStr`. */
    void replace(std::string &str, const std::string &oldStr, const std::string &newStr);

    /** Returns true if `str` begins with the string `prefix`. */
    bool hasPrefix(const std::string &str, const std::string &prefix) noexcept;

    /** Returns true if `str` ends with the string `prefix`. */
    bool hasSuffix(const std::string &str, const std::string &suffix) noexcept;

    /** Returns true if `str` ends with the string `prefix`, treating ASCII upper/lower case
        letters as equivalent. */
    bool hasSuffixIgnoringCase(const std::string &str, const std::string &suffix) noexcept;

    /** Compares strings, treating ASCII upper/lowercase letters equivalent. Returns -1, 0 or 1. */
    int compareIgnoringCase(const std::string &a, const std::string &b);

    /** Converts an ASCII string to lowercase, in place. */
    void toLowercase(std::string &);

    //////// UNICODE_AWARE FUNCTIONS:

    /** Returns true if the UTF-8 encoded slice contains no characters with code points < 32. */
    bool hasNoControlCharacters(fleece::slice) noexcept;

    /** Returns true if the UTF-8 encoded string contains no characters with code points < 32. */
    static inline bool hasNoControlCharacters(const std::string &str) noexcept {
        return hasNoControlCharacters(fleece::slice(str));
    }

    /** Returns true if the slice contains valid UTF-8 encoded data. */
    bool isValidUTF8(fleece::slice) noexcept;

    /** Returns true if the string contains valid UTF-8 encoded data. */
    static inline bool isValidUTF8(const std::string &str) noexcept {
        return isValidUTF8(fleece::slice(str));
    }

    /** Returns the number of characters in a UTF-8 encoded string. */
    size_t UTF8Length(fleece::slice) noexcept;

    /** Returns the byte length of the next UTF-8 character in a UTF-8 encoded string, or 0 if invalid */
    size_t NextUTF8Length(fleece::slice) noexcept;

    /** Returns a slice containing the bytes of the next UTF-8 encoded character, or nullslice if
     *  not valid or no more characters remain */
    fleece::pure_slice NextUTF8(fleece::slice) noexcept;

    /** Returns a copy of a UTF-8 string with all letters converted to upper- or lowercase.
        This function is Unicode-aware and will convert non-ASCII letters.
        It returns a null slice if the input is invalid UTF-8. */
    fleece::alloc_slice UTF8ChangeCase(fleece::slice str, bool toUppercase);

    /** Trims Unicode whitespace characters from one or both ends of the string by updating
        `chars` and/or `count`.
        `onSide` should be negative for left, 0 for both sides, positive for right. */
    void UTF16Trim(const char16_t* &chars, size_t &count, int onSide) noexcept;

    /** Returns true if `c` is a Unicode whitespace character. */
    bool UTF16IsSpace(char16_t c) noexcept;

}


// Utility for using slice with printf-style formatting.
// Use "%.*" in the format string; then for the corresponding argument put SPLAT(theslice).
#define SPLAT(S)    (int)(S).size, (char *)(S).buf
