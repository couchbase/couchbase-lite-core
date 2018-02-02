//
// civetUtils.cc
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

#include "civetUtils.hh"
#include "c4.h"
#include "PlatformIO.hh"
#include "civetweb.h"
#include <assert.h>


#ifndef NDEBUG
// Also declared in civetweb.pch
extern "C" void lc_civet_trace(const char *func, unsigned line, const char *fmt, ...);

void lc_civet_trace(const char *func, unsigned line, const char *fmt, ...) {
    char *message;
    va_list args;
    va_start(args, fmt);
    vasprintf(&message, fmt, args);
    va_end(args);

    c4log(c4log_getDomain("REST", true), kC4LogDebug, "%s  (%s:%u)", message, func, line);
    free(message);
}
#endif


namespace litecore { namespace REST {


    void mg_strlcpy(register char *dst, register const char *src, size_t n)
    {
        for (; *src != '\0' && n > 1; n--) {
            *dst++ = *src++;
        }
        *dst = '\0';
    }

    /* Convert time_t to a string. According to RFC2616, Sec 14.18, this must be
     * included in all responses other than 100, 101, 5xx. */
    void gmt_time_string(char *buf, size_t buf_len, time_t *t)
    {
        struct tm *tm;

        tm = ((t != NULL) ? gmtime(t) : NULL);
        if (tm != NULL) {
            strftime(buf, buf_len, "%a, %d %b %Y %H:%M:%S GMT", tm);
        } else {
            mg_strlcpy(buf, "Thu, 01 Jan 1970 00:00:00 GMT", buf_len);
            buf[buf_len - 1] = '\0';
        }
    }


    void urlDecode(const char *src,
                    size_t src_len,
                    std::string &dst,
                    bool is_form_url_encoded)
    {
        int i, j, a, b;
#define HEXTOI(x) (isdigit(x) ? x - '0' : x - 'W')

        dst.clear();
        for (i = j = 0; i < (int)src_len; i++, j++) {
            if (i < (int)src_len - 2 && src[i] == '%'
                && isxdigit(*(const unsigned char *)(src + i + 1))
                && isxdigit(*(const unsigned char *)(src + i + 2))) {
                a = tolower(*(const unsigned char *)(src + i + 1));
                b = tolower(*(const unsigned char *)(src + i + 2));
                dst.push_back((char)((HEXTOI(a) << 4) | HEXTOI(b)));
                i += 2;
            } else if (is_form_url_encoded && src[i] == '+') {
                dst.push_back(' ');
            } else {
                dst.push_back(src[i]);
            }
        }
    }


    void urlEncode(const char *src,
                    size_t src_len,
                    std::string &dst,
                    bool append)
    {
        static const char *dont_escape = "._-$,;~()";
        static const char *hex = "0123456789abcdef";

        if (!append)
            dst.clear();

        for (; src_len > 0; src++, src_len--) {
            if (isalnum(*(const unsigned char *)src)
                || strchr(dont_escape, *(const unsigned char *)src) != NULL) {
                dst.push_back(*src);
            } else {
                dst.push_back('%');
                dst.push_back(hex[(*(const unsigned char *)src) >> 4]);
                dst.push_back(hex[(*(const unsigned char *)src) & 0xf]);
            }
        }
    }


    bool getParam(const char *data,
                   size_t data_len,
                   const char *name,
                   std::string &dst,
                   size_t occurrence)
    {
        const char *p, *e, *s;
        size_t name_len;

        dst.clear();
        if (data == NULL || name == NULL || data_len == 0) {
            return false;
        }
        name_len = strlen(name);
        e = data + data_len;

        // data is "var1=val1&var2=val2...". Find variable first
        for (p = data; p + name_len < e; p++) {
            if ((p == data || p[-1] == '&') && p[name_len] == '='
                && !mg_strncasecmp(name, p, name_len) && 0 == occurrence--) {

                // Point p to variable value
                p += name_len + 1;

                // Point s to the end of the value
                s = (const char *)memchr(p, '&', (size_t)(e - p));
                if (s == NULL) {
                    s = e;
                }
                assert(s >= p);

                // Decode variable into destination buffer
                urlDecode(p, (int)(s - p), dst, true);
                return true;
            }
        }
        return false;
    }

} }


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
