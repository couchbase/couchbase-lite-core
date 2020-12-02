//
// TestUtils.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "TestUtils.hh"
#include "fleece/Fleece.h"
#include "catch.hpp"

using namespace std;
using namespace fleece;


string sliceToHex(slice result) {
    string hex;
    for (size_t i = 0; i < result.size; i++) {
        char str[4];
        sprintf(str, "%02X", result[i]);
        hex.append(str);
        if (i % 2 && i != result.size-1)
            hex.append(" ");
    }
    return hex;
}


string sliceToHexDump(slice result, size_t width) {
    string hex;
    for (size_t row = 0; row < result.size; row += width) {
        size_t end = min(row + width, result.size);
        for (size_t i = row; i < end; ++i) {
            char str[4];
            sprintf(str, "%02X", result[i]);
            hex.append(str);
            if (i % 2 && i != result.size-1)
                hex.append(" ");
        }
        hex.append("    ");
        for (size_t i = row; i < end; ++i) {
            char str[2] = {(char)result[i], 0};
            if (result[i] < 32 || result[i] >= 127)
                str[0] = '.';
            hex.append(str);
        }
        hex.append("\n");
    }
    return hex;
}


namespace fleece {
    ostream& operator<< (ostream& o, pure_slice s) {
        o << "slice[";
        if (s.buf == nullptr)
            return o << "null]";
        auto buf = (const uint8_t*)s.buf;
        for (size_t i = 0; i < s.size; i++) {
            if (buf[i] < 32 || buf[i] > 126)
                return o << sliceToHex(slice(s)) << "]";
        }
        return o << "\"" << string((char*)s.buf, s.size) << "\"]";
    }
}


fleece::alloc_slice json5slice(string_view str) {
    FLStringResult errorMsg = {};
    size_t errorPos = 0;
    FLError err;
    auto json = alloc_slice(FLJSON5_ToJSON(slice(str), &errorMsg, &errorPos, &err));
    INFO("JSON5 error: " << string(alloc_slice(errorMsg)) << ", input was: " << str);
    REQUIRE(json.buf);
    return json;
}


string json5(string_view str) {
    return string(json5slice(str));
}


