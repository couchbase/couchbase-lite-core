//
// netUtils.cc
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
//  This code is adapted from civetweb.c and CivetServer.cpp; see original license at end of file.

#include "netUtils.hh"
#include "NumConversion.hh"
#include <cassert>
#include <ctime>
#include "StringUtil.hh"
#ifndef __APPLE__
#    include "PlatformIO.hh"
#endif

namespace litecore::REST {
    using namespace std;
    using namespace fleece;

    string URLDecode(slice src, bool isFormURLEncoded) {
        string dst;
        dst.reserve(src.size);
        for ( size_t i = 0; i < src.size; i++ ) {
            if ( i < src.size - 2 && src[i] == '%' && isxdigit(src[i + 1]) && isxdigit(src[i + 2]) ) {
                int a = digittoint(src[i + 1]);
                int b = digittoint(src[i + 2]);
                dst.push_back((char)((a << 4) | b));
                i += 2;
            } else if ( isFormURLEncoded && src[i] == '+' ) {
                dst.push_back(' ');
            } else {
                dst.push_back(narrow_cast<char>(src[i]));
            }
        }
        return dst;
    }

    static void urlEncode(const unsigned char* src, size_t src_len, std::string& dst, bool append) {
        static const char* dont_escape = "._-$,;~()";
        static const char* hex         = "0123456789abcdef";

        if ( !append ) {
            dst.clear();
            dst.reserve(src_len);
        }

        for ( ; src_len > 0; src++, src_len-- ) {
            if ( isalnum(*src) || strchr(dont_escape, *src) != nullptr ) {
                dst.push_back(narrow_cast<char>(*src));
            } else {
                dst.push_back('%');
                dst.push_back(hex[(*src) >> 4]);
                dst.push_back(hex[(*src) & 0xf]);
            }
        }
    }

    string URLEncode(slice str) {
        string result;
        urlEncode((const unsigned char*)str.buf, str.size, result, false);
        return result;
    }

    bool iterateURLQueries(string_view queries, char delimiter, function_ref<bool(string_view, string_view)> callback) {
        bool stop = false;
        split(queries, string_view{&delimiter, 1}, [&](string_view query) {
            if ( !stop ) {
                string value;
                if ( auto eq = query.find('='); eq != string::npos ) {
                    value = query.substr(eq + 1);
                    query = query.substr(0, eq);
                }
                stop = callback(query, value);
            }
        });
        return stop;
    }

    string getURLQueryParam(slice queries, string_view name, char delimiter, size_t occurrence) {
        string value;
        iterateURLQueries(queries, delimiter, [&](string_view k, string_view v) -> bool {
            if ( name.size() == k.size() && 0 == strncasecmp(name.data(), k.data(), k.size()) && 0 == occurrence-- ) {
                value = URLDecode(v);
                return true;  // stop iteration
            }
            return false;
        });
        return value;
    }

}  // namespace litecore::REST

/* Copyright (c) 2013-2017 the Civetweb developers
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
