//
//  LogEncoder.cc
//  Fleece
//
//  Created by Jens Alfke on 5/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "Endian.hh"
#include "varint.hh"
#include <exception>
#include <time.h>

using namespace std;
using namespace fleece;

namespace litecore {

    const uint8_t LogEncoder::kMagicNumber[4] = {0xcf, 0xb2, 0xab, 0x1b};

    // The units we count in are microseconds.
    static constexpr unsigned kTicksPerSec = 1000000;

    // Log will be written to output stream when this much has been captured:
    static const size_t kBufferSize = 64 * 1024;

    // ...or when this many seconds have elapsed since the previous save:
    static const uint64_t kSaveInterval = 5 * kTicksPerSec;


    LogEncoder::LogEncoder(ostream &out)
    :_out(out)
    {
        _writer.write(&kMagicNumber, 4);
        uint8_t header[2] = {kFormatVersion, sizeof(void*)};
        _writer.write(&header, sizeof(header));
        auto now = LogDecoder::now();
        writeUVarInt(now.secs);
        _lastElapsed = -(int)now.microsecs;  // so first delta will be accurate
        _st.reset();
    }


    LogEncoder::~LogEncoder() {
        flush();
    }


    void LogEncoder::log(int8_t level, const char *domain, ObjectRef object, const char *format, ...) {
        va_list args;
        va_start(args, format);
        vlog(level, domain, object, format, args);
        va_end(args);
    }


    void LogEncoder::vlog(int8_t level, const char *domain, ObjectRef object, const char *format, va_list args) {
        // Write the number of ticks elapsed since the last message:
        auto elapsed = int64_t(_st.elapsed() * kTicksPerSec);
        uint64_t delta = elapsed - _lastElapsed;
        _lastElapsed = elapsed;
        writeUVarInt(delta);

        // Write level, domain, format string:
        _writer.write(&level, sizeof(level));
        writeStringToken(domain ? domain : "");

        writeUVarInt((unsigned)object);
        if (object != ObjectRef::None) {
            auto i = _objects.find(unsigned(object));
            if (i != _objects.end()) {
                _writer.write(slice(i->second));
                _writer.write("\0", 1);
                _objects.erase(i);
            }
        }

        writeStringToken(format);

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
                        writeUVarInt(abs(param));
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
                        writeUVarInt(param);
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
                            writeStringToken(str);
                        } else {
                            writeUVarInt(size);
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
                    case '%':
                        break;
                    default:
                        throw invalid_argument("Unknown type in LogEncoder format string");
                }
            }
        }
        if (_writer.length() > kBufferSize || elapsed - _lastSaved > kSaveInterval)
            flush();
    }


    LogEncoder::ObjectRef LogEncoder::registerObject(std::string description) {
        ObjectRef ref = _lastObjectRef = ObjectRef(unsigned(_lastObjectRef) + 1);
        _objects[unsigned(ref)] = description;
        return ref;
    }


    void LogEncoder::unregisterObject(ObjectRef obj) {
        _objects.erase(unsigned(obj));
    }


    void LogEncoder::writeUVarInt(uint64_t n) {
        uint8_t buf[kMaxVarintLen64];
        _writer.write(buf, PutUVarInt(buf, n));
    }


    void LogEncoder::writeStringToken(const char *token) {
        auto i = _formats.find((size_t)token);
        if (i == _formats.end()) {
            unsigned n = (unsigned)_formats.size();
            _formats.insert({(size_t)token, n});
            writeUVarInt(n);
            _writer.write(token, strlen(token)+1);  // add the actual string the first time
        } else {
            writeUVarInt(i->second);
        }
    }

    
    void LogEncoder::flush() {
        for (slice s : _writer.output())
            _out.write((const char*)s.buf, s.size);
        _writer.reset();
        _out.flush();
        _lastSaved = _lastElapsed;
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
