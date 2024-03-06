//
// StringUtil.hh
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
#include "fleece/function_ref.hh"
#include "fleece/slice.hh"
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <sstream>
#include <string_view>
#ifndef __printflike
#    include "c4Compat.h"
#endif

namespace litecore {

    using namespace fleece;

#if defined(__ANDROID__) || defined(__GLIBC__) || defined(_MSC_VER)
    // Converts a decimal or hex digit to its integer equivalent (0..15), or 0 if not a digit.
    // (This function is part of <ctype.h> in BSD and Apple OSs.)
    int digittoint(char ch);
#endif

    // Adds EXPR to a stringstream and returns the resulting string.
    // Example: CONCAT("2+2=" << 4 << "!") --> "2+2=4!"
#ifndef _LIBCPP_VERSION
#    define CONCAT(EXPR) (static_cast<const std::stringstream&>(std::stringstream() << EXPR)).str()
#else
#    define CONCAT(EXPR) (std::stringstream() << EXPR).str()
#endif

    /** Writes a slice to a stream with the usual "<<" syntax */
    static inline std::ostream& operator<<(std::ostream& o, fleece::slice s) {
        o.write((const char*)s.buf, static_cast<std::streamsize>(s.size));
        return o;
    }

    /** Like sprintf(), but returns a std::string */
    std::string format(const char* fmt NONNULL, ...) __printflike(1, 2);

    std::string format(std::string fmt, ...);

    /** Like vsprintf(), but returns a std::string */
    std::string vformat(const char* fmt NONNULL, va_list) __printflike(1, 0);

    void split(std::string_view str, std::string_view separator, fleece::function_ref<void(std::string_view)> callback);

    /** Returns the strings in the vector concatenated together,
        with the separator (if non-null) between them. */
    std::string join(const std::vector<std::string>&, const char* separator = nullptr);

    /** Concatenates the strings in the vector onto the stream,
        with the separator (if non-null) between them. */
    std::stringstream& join(std::stringstream&, const std::vector<std::string>&, const char* separator = nullptr);

    /** Removes last character from string (in place.) Does nothing if string is empty. */
    void chop(std::string&) noexcept;

    /** Removes last character from string (in place), but only if it equals `ending` */
    void chomp(std::string&, char ending) noexcept;

    /** Replaces all occurrences of `oldChar` with `newChar`. */
    void replace(std::string& str, char oldChar, char newChar);

    /** Replaces all occurrences of `oldStr` with `newStr`. */
    void replace(std::string& str, std::string_view oldStr, std::string_view newStr);

    /** Returns true if `str` begins with the string `prefix`. */
    bool hasPrefix(std::string_view str, std::string_view prefix) noexcept;

    /** Returns true if `str` ends with the string `prefix`. */
    bool hasSuffix(std::string_view str, std::string_view suffix) noexcept;

    /** Returns true if `str` ends with the string `prefix`, treating ASCII upper/lower case
        letters as equivalent. */
    bool hasSuffixIgnoringCase(std::string_view str, std::string_view suffix) noexcept;

    /** Compares strings, treating ASCII upper/lowercase letters equivalent. Returns -1, 0 or 1. */
    int compareIgnoringCase(const std::string& a, const std::string& b);

    /** Converts an ASCII string to lowercase, in place. */
    void toLowercase(std::string&);

    static inline std::string lowercase(std::string str) {
        toLowercase(str);
        return str;
    }

    /** Returns true if `str` matches the pattern `pattern`, which uses typical (Unix) shell
        wildcard syntax: `?` matches a single character, `*` matches any number of characters,
        `[...]` matches a specific character `\\` escapes the next character.
        See <https://man7.org/linux/man-pages/man7/glob.7.html> for details.*/
    bool matchGlobPattern(const std::string &str, const std::string &pattern);

    //////// UNICODE_AWARE FUNCTIONS:

    /** Returns true if the UTF-8 encoded slice contains no characters with code points < 32. */
    bool hasNoControlCharacters(fleece::slice) noexcept;

    /** Returns true if the UTF-8 encoded string contains no characters with code points < 32. */
    static inline bool hasNoControlCharacters(const std::string& str) noexcept {
        return hasNoControlCharacters(fleece::slice(str));
    }

    /** Returns true if the slice contains valid UTF-8 encoded data. */
    bool isValidUTF8(fleece::slice) noexcept;

    /** Returns true if the string contains valid UTF-8 encoded data. */
    static inline bool isValidUTF8(const std::string& str) noexcept { return isValidUTF8(fleece::slice(str)); }

    /** Returns the number of characters in a UTF-8 encoded string. */
    size_t UTF8Length(fleece::slice) noexcept;

    /** Returns the byte length of the next UTF-8 character in a UTF-8 encoded string, or 0 if invalid */
    size_t NextUTF8Length(fleece::slice) noexcept;

    /** Returns a slice containing the bytes of the next UTF-8 encoded character, or nullslice if
     *  not valid or no more characters remain */
    fleece::slice NextUTF8(fleece::slice) noexcept;

    /** Returns a copy of a UTF-8 string with all letters converted to upper- or lowercase.
        This function is Unicode-aware and will convert non-ASCII letters.
        It returns a null slice if the input is invalid UTF-8. */
    fleece::alloc_slice UTF8ChangeCase(fleece::slice str, bool toUppercase);

    /** Trims Unicode whitespace characters from one or both ends of the string by updating
        `chars` and/or `count`.
        `onSide` should be negative for left, 0 for both sides, positive for right. */
    void UTF16Trim(const char16_t*& chars, size_t& count, int onSide) noexcept;

    /** Returns true if `c` is a Unicode whitespace character. */
    bool UTF16IsSpace(char16_t c) noexcept;

}  // namespace litecore

// Utility for using slice with printf-style formatting.
// Use "%.*" in the format string; then for the corresponding argument put SPLAT(theslice).
#define SPLAT(S) (int)(S).size, (char*)(S).buf
