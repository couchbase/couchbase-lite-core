//
// StringUtil.cc
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

#include "StringUtil.hh"
#include "Logging.hh"
#include "PlatformIO.hh"
#include <sstream>
#include <stdlib.h>

namespace litecore {

    using namespace std;
    using namespace fleece;

    std::string format(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        std::string result = vformat(fmt, args);
        va_end(args);
        return result;
    }


    std::string vformat(const char *fmt, va_list args) {
        char *cstr = nullptr;
        vasprintf(&cstr, fmt, args);
        std::string result(cstr);
        free(cstr);
        return result;
    }


    stringstream& join(stringstream &s, const std::vector<std::string> &strings, const char *separator) {
        int n = 0;
        for (const string &str : strings) {
            if (n++ && separator)
                s << separator;
            s << str;
        }
        return s;
    }

    std::string join(const std::vector<std::string> &strings, const char *separator) {
        stringstream s;
        return join(s, strings, separator).str();
    }

    void chop(std::string &str) noexcept {
        auto sz = str.size();
        if (sz > 0)
            str.resize(sz - 1);
    }

    void chomp(std::string &str, char ending) noexcept {
        auto sz = str.size();
        if (sz > 0 && str[sz - 1] == ending)
            str.resize(sz - 1);
    }


    void replace(std::string &str, char oldChar, char newChar) {
        for (char &c : str)
            if (c == oldChar)
                c = newChar;
    }


    bool hasPrefix(const std::string &str, const std::string &prefix) noexcept {
        return str.size() >= prefix.size() && memcmp(str.data(), prefix.data(), prefix.size()) == 0;
    }

    bool hasSuffix(const std::string &str, const std::string &suffix) noexcept {
        return str.size() >= suffix.size()
            && memcmp(&str[str.size() - suffix.size()], suffix.data(), suffix.size()) == 0;
    }

    bool hasSuffixIgnoringCase(const std::string &str, const std::string &suffix) noexcept {
        return str.size() >= suffix.size()
            && strcasecmp(&str.c_str()[str.size() - suffix.size()], suffix.data()) == 0;
    }

    int compareIgnoringCase(const std::string &a, const std::string &b) {
        return strcasecmp(a.c_str(), b.c_str());
    }

    void toLowercase(std::string &str) {
        for (char &c : str)
            c = (char)tolower(c);
    }

    // Based on utf8_check.c by Markus Kuhn, 2005
    // https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
    bool isValidUTF8(fleece::slice sl) noexcept
    {
        auto s = (const uint8_t*)sl.buf;
        for (auto e = s + sl.size; s != e; ) {
            while (!(*s & 0x80)) {
                if (++s == e) {
                    return true;
                }
            }

            if ((s[0] & 0x60) == 0x40) {
                if (s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0) {
                    return false;
                }
                s += 2;
            } else if ((s[0] & 0xf0) == 0xe0) {
                if (s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
                    (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || (s[0] == 0xed && (s[1] & 0xe0) == 0xa0)) {
                    return false;
                }
                s += 3;
            } else if ((s[0] & 0xf8) == 0xf0) {
                if (s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80 ||
                    (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) || (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4) {
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
        for (auto e = s + sl.size; s != e; ++s) {
            if (_usuallyFalse(*s < 32))
                return false;
            else if (_usuallyFalse(*s == 0xC0 && s + 1 != e && s[1] == 0x80)) // UTF-8 encoded NUL
                return false;
        }
        return true;
    }

    size_t UTF8Length(slice str) noexcept {
        // See <https://en.wikipedia.org/wiki/UTF-8>
        size_t length = 0;
        auto s = (const uint8_t*)str.buf, e = (const uint8_t*)str.end();
        while (s < e) {
            uint8_t c = *s;
            if (_usuallyTrue((c & 0x80) == 0))
                s += 1;
            else if (_usuallyTrue((c & 0xE0) == 0xC0))
                s += 2;
            else if ((c & 0xF0) == 0xE0)
                s += 3;
            else if ((c & 0xF8) == 0xF0)
                s += 4;
            else
                s += 1;        // Invalid byte; skip over it
            ++length;
        }
        return length;
    }

    bool UTF16IsSpace(char16_t c) noexcept {
        // "ISO 30112 defines POSIX space characters as Unicode characters U+0009..U+000D, U+0020,
        // U+1680, U+180E, U+2000..U+2006, U+2008..U+200A, U+2028, U+2029, U+205F, and U+3000."
        if (c <= 0x20)
            return c == 0x20 || (c >= 0x09 && c <= 0x0D);
        else if (_usuallyTrue(c < 0x1680))
            return false;
        else
            return c == 0x1680 || c == 0x180E || (c >= 0x2000 && c <= 0x200A && c != 0x2007)
                || c == 0x2028 || c == 0x2029 || c == 0x205F || c == 0x3000;
    }


    void UTF16Trim(const char16_t* &chars, size_t &count, int side) noexcept {
        if (side <= 0) {
            while (count > 0 && UTF16IsSpace(*chars)) {
                ++chars;
                --count;
            }
        }
        if (side >= 0) {
            auto last = chars + count - 1;
            while (count > 0 && UTF16IsSpace(*last)) {
                --last;
                --count;
            }
        }
    }


#if !__APPLE__ && !defined(_MSC_VER) && !LITECORE_USES_ICU    // TODO: Full implementation of UTF8ChangeCase for other platforms (see StringUtil_Apple.mm)

    // Stub implementation for when case conversion is unavailable
    alloc_slice UTF8ChangeCase(slice str, bool toUppercase) {
        auto f = toUppercase ? toupper : tolower;
        auto size = str.size;
        alloc_slice result(size);
        for (size_t i = 0; i < size; i++)
            (uint8_t&)(result[i]) = (uint8_t) f(str[i]);
        return result;
    }

#endif

}
