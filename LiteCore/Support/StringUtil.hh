//
//  StringUtil.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/23/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include <ciso646> // define _LIBCPP_VERSION to check if we need Android log2
#include "PlatformCompat.hh"
#include "slice.hh"
#include <stdarg.h>
#include <string>

namespace litecore {

    /** Like sprintf(), but returns a std::string */
    std::string format(const char *fmt, ...) __printflike(1, 2);

    /** Like vsprintf(), but returns a std::string */
    std::string vformat(const char *fmt, va_list);

    /** Removes last character from string (in place.) Does nothing if string is empty. */
    void chop(std::string&) noexcept;

    /** Removes last character from string (in place), but only if it equals `ending` */
    void chomp(std::string&, char ending) noexcept;

    bool hasPrefix(const std::string &str, const std::string &prefix) noexcept;

    bool hasSuffix(const std::string &str, const std::string &suffix) noexcept;

    bool isValidUTF8(fleece::slice) noexcept;

    static inline bool isValidUTF8(const std::string &str) noexcept {
        return isValidUTF8(fleece::slice(str));
    }

    bool hasNoControlCharacters(fleece::slice) noexcept;

    static inline bool hasNoControlCharacters(const std::string &str) noexcept {
        return hasNoControlCharacters(fleece::slice(str));
    }
}


// Utility for using slice with printf-style formatting.
// Use "%.*" in the format string; then for the corresponding argument put SPLAT(theslice).
#define SPLAT(S)    (int)(S).size, (S).buf
