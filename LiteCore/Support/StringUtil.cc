//
//  StringUtil.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/23/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "StringUtil.hh"
#include <stdlib.h>

#ifdef _MSC_VER
#include "asprintf.h"
#else
#include <unistd.h>
#endif

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


    void chop(std::string &str) {
        auto sz = str.size();
        if (sz > 0)
            str.resize(sz - 1);
    }

    void chomp(std::string &str, char ending) {
        auto sz = str.size();
        if (sz > 0 && str[sz - 1] == ending)
            str.resize(sz - 1);
    }

}
