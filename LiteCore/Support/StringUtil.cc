//
//  StringUtil.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/23/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "StringUtil.hh"
#include "PlatformIO.hh"
#include <stdlib.h>

namespace litecore {

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

    // Based on utf8_check.c by Markus Kuhn, 2005
    // https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
    // Optimized for predominantly 7-bit content, 2016
    bool isValidUTF8(fleece::slice sl) noexcept
    {
        auto s = (const uint8_t*)sl.buf;
        for (auto e = s + sl.size; s != e; ) {
            if (s + 4 <= e && ((*(uint32_t *) s) & 0x80808080) == 0) {
                s += 4;
            } else {
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
        }
        return true;
    }

    bool hasNoControlCharacters(fleece::slice sl) noexcept {
        auto s = (const uint8_t*)sl.buf;
        for (auto e = s + sl.size; s != e; ++s) {
            if (*s < 32)
                return false;
        }
        return true;
    }

}
