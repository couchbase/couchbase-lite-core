//
// LogEncoder.cc
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

#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "Endian.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include <exception>
#include <iostream>
#include <time.h>

#if __APPLE__
#import <CoreFoundation/CFBase.h>
#import <CoreFoundation/CFString.h>
#endif

using namespace std;
using namespace fleece;

namespace litecore {

    const uint8_t LogEncoder::kMagicNumber[4] = {0xcf, 0xb2, 0xab, 0x1b};

    // The units we count in are microseconds.
    static constexpr unsigned kTicksPerSec = 1000000;

    // Log will be written to output stream when this much has been captured:
    static const size_t kBufferSize = 64 * 1024;

    // ...or when this many seconds have elapsed since the previous save:
    static const uint64_t kSaveInterval = 1 * kTicksPerSec;


    LogEncoder::LogEncoder(ostream &out, LogLevel level)
    :_out(out)
    ,_flushTimer(bind(&LogEncoder::performScheduledFlush, this))
    ,_level(level)
    {
        _writer.write(&kMagicNumber, 4);
        uint8_t header[2] = {kFormatVersion, sizeof(void*)};
        _writer.write(&header, sizeof(header));
        auto now = LogDecoder::now();
        _writeUVarInt(now.secs);
        _lastElapsed = -(int)now.microsecs;  // so first delta will be accurate
        _st.reset();
    }

    LogEncoder::~LogEncoder() {
        flush();
    }


#pragma mark - LOGGING:


    void LogEncoder::log(const char *domain, ObjectRef object, const char *format, ...) {
        va_list args;
        va_start(args, format);
        vlog(domain, object, format, args);
        va_end(args);
    }


    int64_t LogEncoder::_timeElapsed() const {
        return int64_t(_st.elapsed() * kTicksPerSec);
    }

    void LogEncoder::vlog(const char *domain, ObjectRef object, const char *format, va_list args) {
        lock_guard<mutex> lock(_mutex);

        // Write the number of ticks elapsed since the last message:
        auto elapsed = _timeElapsed();
        uint64_t delta = elapsed - _lastElapsed;
        _lastElapsed = elapsed;
        _writeUVarInt(delta);

        // Write level, domain, format string:
        _writer.write(&_level, sizeof(_level));
        _writeStringToken(domain ? domain : "");

        const auto objRef = (unsigned)object;
        _writeUVarInt(objRef);
        if (object != ObjectRef::None && _seenObjects.find(objRef) == _seenObjects.end()) {
            _seenObjects.insert(objRef);
            auto i = LogDomain::getObject(objRef);
            _writer.write(slice(i));
            _writer.write("\0", 1);
        }

        _writeStringToken(format);

        // Parse the format string looking for substitutions:
        for (const char *c = format; *c != '\0'; ++c) {
            if (*c == '%') {
                bool minus = false;
                bool dotStar = false;
                ++c;
                if (*c == '-') {
                    minus = true;
                    ++c;
                }
                c += strspn(c, "#0- +'");
                while (isdigit(*c))
                    ++c;
                if (*c == '.') {
                    ++c;
                    if (*c == '*') {
                        dotStar = true;
                        ++c;
                    } else {
                        while (isdigit(*c))
                            ++c;
                    }
                }
                c += strspn(c, "hljtzq");

                switch(*c) {
                    case 'c':
                    case 'd':
                    case 'i': {
                        long long param;
                        if (c[-1] == 'q')
                            param = va_arg(args, long long);
                        else if (c[-1] == 'z')
                            param = va_arg(args, ptrdiff_t);
                        else if (c[-1] != 'l')
                            param = va_arg(args, int);
                        else if (c[-2] != 'l')
                            param = va_arg(args, long);
                        else
                            param = va_arg(args, long long);
                        uint8_t sign = (param < 0) ? 1 : 0;
                        _writer.write(&sign, 1);
                        _writeUVarInt(abs(param));
                        break;
                    }
                    case 'u':
                    case 'x': case 'X': {
                        unsigned long long param;
                        if (c[-1] == 'q')
                            param = va_arg(args, unsigned long long);
                        else if (c[-1] == 'z')
                            param = va_arg(args, size_t);
                        else if (c[-1] != 'l')
                            param = va_arg(args, unsigned int);
                        else if (c[-2] != 'l')
                            param = va_arg(args, unsigned long);
                        else
                            param = va_arg(args, unsigned long long);
                        _writeUVarInt(param);
                        break;
                    }
                    case 'e': case 'E':
                    case 'f': case 'F':
                    case 'g': case 'G':
                    case 'a': case 'A': {
                        littleEndianDouble param = va_arg(args, double);
                        _writer.write(&param, sizeof(param));
                        break;
                    }
                    case 's': {
                        const char *str;
                        size_t size;
                        if (dotStar) {
                            size = va_arg(args, int);
                            str = va_arg(args, const char*);
                        } else {
                            str = va_arg(args, const char*);
                            size = strlen(str);
                        }
                        if (minus && !dotStar) {
                            _writeStringToken(str);
                        } else {
                            _writeUVarInt(size);
                            _writer.write(str, size);
                        }
                        break;
                    }
                    case 'p': {
                        size_t param = va_arg(args, size_t);
                        if (sizeof(param) == 8)
                            param = _encLittle64(param);
                        else
                            param = _encLittle32(param);
                        _writer.write(&param, sizeof(param));
                        break;
                    }
#if __APPLE__
                    case '@': {
                        // "%@" substitutes an Objective-C or CoreFoundation object's description.
                        CFTypeRef param = va_arg(args, CFTypeRef);
                        if (param == nullptr) {
                            _writeUVarInt(6);
                            _writer.write("(null)", 6);
                        } else {
                            CFStringRef description;
                            if (CFGetTypeID(param) == CFStringGetTypeID())
                                description = (CFStringRef)param;
                            else
                                description = CFCopyDescription(param);
                            nsstring_slice descSlice(description);
                            _writeUVarInt(descSlice.size);
                            _writer.write(descSlice);
                            if (description != param)
                                CFRelease(description);
                        }
                        break;
                    }
#endif
                    case '%':
                        break;
                    default:
                        throw invalid_argument("Unknown type in LogEncoder format string");
                }
            }
        }

        if (_writer.length() > kBufferSize)
            _flush();
        else
            _scheduleFlush();
    }

    void LogEncoder::_writeUVarInt(uint64_t n) {
        uint8_t buf[kMaxVarintLen64];
        _writer.write(buf, PutUVarInt(buf, n));
    }


    void LogEncoder::_writeStringToken(const char *token) {
        const auto name = _formats.find((size_t)token);
        if (name == _formats.end()) {
            const auto n = (unsigned)_formats.size();
            _formats.insert({(size_t)token, n});
            _writeUVarInt(n);
            _writer.write(token, strlen(token)+1);  // add the actual string the first time
        } else {
            _writeUVarInt(name->second);
        }
    }


#pragma mark - FLUSHING:


    void LogEncoder::flush() {
        lock_guard<mutex> lock(_mutex);
        _flush();
    }
    
    void LogEncoder::_flush() {
        if (_writer.length() == 0)
            return;

        for (slice s : _writer.output())
            _out << s;
        _writer.reset();
        _out.flush();
        _lastSaved = _lastElapsed;
    }


    void LogEncoder::_scheduleFlush() {
        if (!_flushTimer.scheduled()) {
            _flushTimer.fireAfter(std::chrono::microseconds(kSaveInterval));
        }
    }


    // This is called on a background thread by the Timer
    void LogEncoder::performScheduledFlush() {
        lock_guard<mutex> lock(_mutex);

        // Don't flush if there's already been a flush since the timer started:
        auto timeSinceSave = _timeElapsed() - _lastSaved;
        if (timeSinceSave >= kSaveInterval) {
            _flush();
        } else {
            _flushTimer.fireAfter(std::chrono::microseconds(kSaveInterval - timeSinceSave));
        }
    }

}

/* FILE FORMAT:
 
 The file header is:
     Magic number:                  CF B2 AB 1B
     Version number:                [byte]              // See kFormatVersion in the header file
     Pointer size:                  [byte]              // 04 or 08
     Starting timestamp (time_t):   [varint]
 
 Each logged line contains:
    Microsecs since last line:      [varint]
    Severity level:                 [byte]              // {debug=0, verbose, info, warning, error}
    Domain ID                       [varint]            // Numbered sequentially starting at 0
        name of domain (1st time)   [nul-terminated string]
    Object ID                       [varint]            // Numbered sequentially starting at 1
        obj description (1st time)  [nul-terminated string]
    Format string                   [nul-terminated string]
    Args                            ...
 
 Formats of arguments, by type:
    unsigned integer, any size      [varint]
    signed integer, any size        [sign byte]              // 0 for positive, 1 for negative
                                    [varint]                 // absolute value
    float, double                   [little-endian 8-byte double]
    string (%s, %.*s)               [varint]                 // size
                                    [bytes]
    tokenized string (%-s)          [varint]                 // token ID, same namespace as domains
                                    [nul-terminated string]  // only on 1st appearance of this ID
    pointer (%p)                    [little-endian integer]  // size is given by Pointer Size header

 The next line begins immediately after the final argument.

 There is no file trailer; EOF comes after the last logged line.
*/
