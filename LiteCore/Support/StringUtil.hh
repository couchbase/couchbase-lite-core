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
#include <stdarg.h>
#include <string>

namespace litecore {

    /** Like sprintf(), but returns a std::string */
    std::string format(const char *fmt, ...) __printflike(1, 2);

    /** Like vsprintf(), but returns a std::string */
    std::string vformat(const char *fmt, va_list);

}


// Utility for using slice with printf-style formatting.
// Use "%.*" in the format string; then for the corresponding argument put SPLAT(theslice).
#define SPLAT(S)    (int)(S).size, (S).buf
