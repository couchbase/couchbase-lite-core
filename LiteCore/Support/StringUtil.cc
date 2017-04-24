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


    bool hasPrefix(const std::string &str, const std::string &prefix) {
        return str.size() >= prefix.size() && memcmp(str.data(), prefix.data(), prefix.size()) == 0;
    }

    bool hasSuffix(const std::string &str, const std::string &suffix) {
        return str.size() >= suffix.size()
            && memcmp(&str[str.size() - suffix.size()], suffix.data(), suffix.size()) == 0;
    }

}
