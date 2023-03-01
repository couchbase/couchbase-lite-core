//
// StringUtil.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "StringUtil.hh"
#include "Logging.hh"
#include "PlatformIO.hh"
#include <sstream>
#include <stdlib.h>

namespace litecore {

    using namespace std;
    using namespace fleece;


#if defined(__ANDROID__) || defined(__GLIBC__) || defined(_MSC_VER)
    // digittoint is a BSD function, not available on Android, Linux, etc.
    int digittoint(char ch) {
        int d = ch - '0';
        if ( (unsigned)d < 10 ) { return d; }
        d = ch - 'a';
        if ( (unsigned)d < 6 ) { return d + 10; }
        d = ch - 'A';
        if ( (unsigned)d < 6 ) { return d + 10; }
        return 0;
    }
#endif  // defined(__ANDROID__) || defined(__GLIBC__)


    std::string format(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::string result = vformat(fmt, args);
        va_end(args);
        return result;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

    std::string format(const std::string fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::string result = vformat(fmt.c_str(), args);
        va_end(args);
        return result;
    }

#pragma GCC diagnostic pop

    std::string vformat(const char* fmt, va_list args) {
        char* cstr = nullptr;
        if ( vasprintf(&cstr, fmt, args) < 0 ) throw bad_alloc();
        std::string result(cstr);
        free(cstr);
        return result;
    }

    void split(string_view str, string_view separator, function_ref<void(string_view)> callback) {
        auto              end = str.size();
        string::size_type pos, next;
        for ( pos = 0; pos < end; pos = next + separator.size() ) {
            next = str.find(separator, pos);
            if ( next == string::npos ) break;
            callback(str.substr(pos, next - pos));
        }
        callback(str.substr(pos));
    }

    stringstream& join(stringstream& s, const std::vector<std::string>& strings, const char* separator) {
        int n = 0;
        for ( const string& str : strings ) {
            if ( n++ && separator ) s << separator;
            s << str;
        }
        return s;
    }

    std::string join(const std::vector<std::string>& strings, const char* separator) {
        stringstream s;
        return join(s, strings, separator).str();
    }

    void chop(std::string& str) noexcept {
        auto sz = str.size();
        if ( sz > 0 ) str.resize(sz - 1);
    }

    void chomp(std::string& str, char ending) noexcept {
        auto sz = str.size();
        if ( sz > 0 && str[sz - 1] == ending ) str.resize(sz - 1);
    }

    void replace(std::string& str, char oldChar, char newChar) {
        for ( char& c : str )
            if ( c == oldChar ) c = newChar;
    }

    void replace(std::string& str, string_view oldStr, string_view newStr) {
        string::size_type pos = 0;
        while ( string::npos != (pos = str.find(oldStr, pos)) ) {
            str.replace(pos, oldStr.size(), newStr);
            pos += newStr.size();
        }
    }

    bool hasPrefix(string_view str, string_view prefix) noexcept {
        return str.size() >= prefix.size() && memcmp(str.data(), prefix.data(), prefix.size()) == 0;
    }

    bool hasSuffix(string_view str, string_view suffix) noexcept {
        return str.size() >= suffix.size()
               && memcmp(&str[str.size() - suffix.size()], suffix.data(), suffix.size()) == 0;
    }

    bool hasSuffixIgnoringCase(string_view str, string_view suffix) noexcept {
        return str.size() >= suffix.size()
               && strncasecmp(&str.data()[str.size() - suffix.size()], suffix.data(), suffix.size()) == 0;
    }

    int compareIgnoringCase(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()); }

    void toLowercase(std::string& str) {
        for ( char& c : str ) c = (char)tolower(c);
    }

    // Based on utf8_check.c by Markus Kuhn, 2005
    // https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
    bool isValidUTF8(fleece::slice sl) noexcept {
        auto s = (const uint8_t*)sl.buf;
        for ( auto e = s + sl.size; s != e; ) {
            while ( !(*s & 0x80) ) {
                if ( ++s == e ) { return true; }
            }

            if ( (s[0] & 0x60) == 0x40 ) {
                if ( s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0 ) { return false; }
                s += 2;
            } else if ( (s[0] & 0xf0) == 0xe0 ) {
                if ( s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80
                     || (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || (s[0] == 0xed && (s[1] & 0xe0) == 0xa0) ) {
                    return false;
                }
                s += 3;
            } else if ( (s[0] & 0xf8) == 0xf0 ) {
                if ( s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80
                     || (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) || (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4 ) {
                    return false;
                }
                s += 4;
            } else {
                return false;
            }
        }
        return true;
    }

    bool hasNoControlCharacters(fleece::slice sl) noexcept {
        auto s = (const uint8_t*)sl.buf;
        for ( auto e = s + sl.size; s != e; ++s ) {
            if ( _usuallyFalse(*s < 32) ) return false;
            else if ( _usuallyFalse(*s == 0xC0 && s + 1 != e && s[1] == 0x80) )  // UTF-8 encoded NUL
                return false;
        }
        return true;
    }

    size_t UTF8Length(slice str) noexcept {
        // See <https://en.wikipedia.org/wiki/UTF-8>
        size_t length = 0;
        while ( str.size > 0 ) {
            const size_t nextByteLength = NextUTF8Length(str);
            if ( nextByteLength == 0 ) {
                str.moveStart(1);
            } else {
                str.moveStart(nextByteLength);
            }
            ++length;
        }
        return length;
    }

    size_t NextUTF8Length(slice str) noexcept {
        if ( str.size == 0 ) { return 0; }

        uint8_t c = *(const uint8_t*)str.buf;
        if ( _usuallyTrue((c & 0x80) == 0) ) return 1;

        if ( _usuallyTrue((c & 0xE0) == 0xC0) ) return _usuallyTrue(str.size > 1) ? 2 : 0;

        if ( (c & 0xF0) == 0xE0 ) return _usuallyTrue(str.size > 2) ? 3 : 0;

        if ( (c & 0xF8) == 0xF0 ) return _usuallyTrue(str.size > 3) ? 4 : 0;

        return 0;
    }

    slice NextUTF8(slice str) noexcept {
        const size_t nextLength = NextUTF8Length(str);
        if ( nextLength == 0 ) { return nullslice; }

        return {str.buf, nextLength};
    }

    bool UTF16IsSpace(char16_t c) noexcept {
        // "ISO 30112 defines POSIX space characters as Unicode characters U+0009..U+000D, U+0020,
        // U+1680, U+180E, U+2000..U+2006, U+2008..U+200A, U+2028, U+2029, U+205F, and U+3000."
        if ( c <= 0x20 ) return c == 0x20 || (c >= 0x09 && c <= 0x0D);
        else if ( _usuallyTrue(c < 0x1680) )
            return false;
        else
            return c == 0x1680 || c == 0x180E || (c >= 0x2000 && c <= 0x200A && c != 0x2007) || c == 0x2028
                   || c == 0x2029 || c == 0x205F || c == 0x3000;
    }

    void UTF16Trim(const char16_t*& chars, size_t& count, int side) noexcept {
        if ( side <= 0 ) {
            while ( count > 0 && UTF16IsSpace(*chars) ) {
                ++chars;
                --count;
            }
        }
        if ( side >= 0 ) {
            auto last = chars + count - 1;
            while ( count > 0 && UTF16IsSpace(*last) ) {
                --last;
                --count;
            }
        }
    }


#if !__APPLE__ && !defined(_MSC_VER) && !LITECORE_USES_ICU

    // Stub implementation for when case conversion is unavailable
    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        auto        size = str.size;
        alloc_slice result(size);
        for ( size_t i = 0; i < size; i++ )
            (uint8_t&)(result[i])
                    = (uint8_t)toUppercase ? toupper((char)str[i], locale()) : tolower((char)str[i], locale());
        return result;
    }

    vector<string> SupportedLocales() { return {}; }

#endif

}  // namespace litecore

// TODO: Is this still needed now that GCC is gone from Android toolchain?
#if defined __ANDROID__ && !defined __clang__
#    include "NumConversion.hh"

namespace std {
    std::string to_string(const double& n) {
        std::ostringstream s;
        s << n;
        return s.str();
    }

    std::string to_string(const long long& n) {
        std::ostringstream s;
        s << n;
        return s.str();
    }

    std::string to_string(const unsigned long long& n) {
        std::ostringstream s;
        s << n;
        return s.str();
    }

    std::string to_string(const uint16_t& n) {
        std::ostringstream s;
        s << n;
        return s.str();
    }

    std::string to_string(const size_t& n) {
        std::ostringstream s;
        s << n;
        return s.str();
    }

    double stod(std::string s) { return ParseDouble(s.c_str()); }

    int stoi(std::string s) { return strtol(s.c_str(), nullptr, 10); }

    long long stoll(std::string s) { return strtoll(s.c_str(), nullptr, 10); }
}  // namespace std
#endif
