//
// LogDecoder.cc
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

#include "LogDecoder.hh"
#include "Endian.hh"
#include <exception>
#include <sstream>
#include <cassert>
#include <cstring>
#include <chrono>
#include <algorithm>
#include "PlatformCompat.hh"

#if __APPLE__
#include <sys/time.h>
#endif

using namespace std;
using namespace fleece;

namespace litecore {

    const uint8_t LogDecoder::kMagicNumber[4] = {0xcf, 0xb2, 0xab, 0x1b};

    // The units we count in are microseconds.
    static constexpr unsigned kTicksPerSec = 1000000;


    /*static*/ LogIterator::Timestamp LogIterator::now() {
        using namespace chrono;

        auto now = time_point_cast<microseconds>(system_clock::now());
        auto count = now.time_since_epoch().count();
        time_t secs = (time_t)count / 1000000;
        unsigned microsecs = count % 1000000;
        return {secs, microsecs};
    }


    void LogIterator::writeTimestamp(Timestamp t, ostream &out) {
        struct tm tm;
        localtime_r(&t.secs, &tm);
        char timestamp[100];
        strftime(timestamp, sizeof(timestamp), "%T", &tm);
        out << timestamp;
        sprintf(timestamp, ".%06u| ", t.microsecs);
        out << timestamp;
    }


    void LogIterator::writeISO8601DateTime(Timestamp t, std::ostream &out) {
        struct tm tm;
        gmtime_r(&t.secs, &tm);
        char timestamp[100];
        strftime(timestamp, sizeof(timestamp), "%FT%T", &tm);
        out << timestamp;
        sprintf(timestamp, ".%06uZ", t.microsecs);
        out << timestamp;
    }


    string LogIterator::formatDate(Timestamp t) {
        struct tm tm;
        localtime_r(&t.secs, &tm);
        char timestamp[100];
        strftime(timestamp, sizeof(timestamp), "%c", &tm);
        return timestamp;
    }


    string LogIterator::readMessage() {
        stringstream out;
        decodeMessageTo(out);
        return out.str();
    }

    
    void LogIterator::decodeTo(ostream &out, const std::vector<std::string> &levelNames) {
        while (next()) {
            writeTimestamp(timestamp(), out);

            string levelName;
            if (level() >= 0 && level() < levelNames.size())
                levelName = levelNames[level()];
            writeHeader(levelName, domain(), out);
            decodeMessageTo(out);
            out << '\n';
        }
    }


    /*static*/ void LogIterator::writeHeader(const string &levelName,
                                            const string &domainName,
                                            ostream &out)
    {
        if (!levelName.empty()) {
            if (!domainName.empty())
                out << '[' << domainName << "] ";
            out << levelName << ": ";
        } else {
            if (!domainName.empty())
                out << '[' << domainName << "]: ";
        }
    }


    LogDecoder::LogDecoder(std::istream &in)
    :_in(in)
    {
        try {
            _in.exceptions(istream::badbit | istream::failbit | istream::eofbit);
            uint8_t header[6];
            _in.read((char*)&header, sizeof(header));
            if (memcmp(&header, &kMagicNumber, 4) != 0)
                throw runtime_error("Not a LiteCore log file");
            if (header[4] != kFormatVersion)
                throw runtime_error("Unsupported log format version");
            _pointerSize = header[5];
            if (_pointerSize != 4 && _pointerSize != 8)
                throw runtime_error("This log file seems to be damaged");
            _startTime = time_t(readUVarInt());
            _readMessage = true;
        } catch (std::ios_base::failure &x) {
            reraise(x);
        }
    }


    bool LogDecoder::next() {
        if (!_readMessage)
            readMessage(); // skip past the unread message

        try {
            _in.exceptions(istream::badbit | istream::failbit);  // turn off EOF exception temporarily
            if (!_in || _in.peek() < 0)
                return false;
            _in.exceptions(istream::badbit | istream::failbit | istream::eofbit);

            _elapsedTicks += readUVarInt();
            _timestamp = {_startTime + time_t(_elapsedTicks / kTicksPerSec),
                          uint32_t(_elapsedTicks % kTicksPerSec)};

            _curLevel = (int8_t)_in.get();
            _curDomain = &readStringToken();

            _curObjectIsNew = false;
            _putCurObjectInMessage = true;
            _curObject = readUVarInt();
            if (_curObject != 0) {
                if (_objects.find(_curObject) == _objects.end()) {
                    _objects.insert({_curObject, readCString()});
                    _curObjectIsNew = true;
                }
            }

            _readMessage = false;
            return true;
        } catch (std::ios_base::failure &x) {
            reraise(x);
        }
    }


    void LogDecoder::decodeTo(ostream &out, const std::vector<std::string> &levelNames) {
        writeTimestamp({_startTime, 0}, out);
        struct tm tm;
        localtime_r(&_startTime, &tm);
        char datestamp[100];
        strftime(datestamp, sizeof(datestamp), "---- Logging begins on %A, %x ----\n", &tm);
        out << datestamp;

        LogIterator::decodeTo(out, levelNames);
    }


#pragma mark - INNARDS:


    uint64_t LogDecoder::objectID() const {
        _putCurObjectInMessage = false;
        return _curObject;
    }


    const string* LogDecoder::objectDescription() const {
        _putCurObjectInMessage = false;
        if (_curObject > 0) {
            auto i = _objects.find(_curObject);
            if (i != _objects.end())
                return &i->second;
        }
        return nullptr;
    }


    void LogDecoder::decodeMessageTo(ostream &out) {
        try {
            assert(!_readMessage);
            _readMessage = true;

            // Write the object ID, unless the caller's already accessed it through the API:
            if (_putCurObjectInMessage && _curObject > 0) {
                out << '{' << _curObject;
                if (_curObjectIsNew)
                    out << "|" << *objectDescription();
                out << "} ";
            }

            // Read the format string, then the parameters:
            string format = readStringToken().c_str();
            for (const char *c = format.c_str(); *c != '\0'; ++c) {
                if (*c != '%') {
                    out << *c;
                } else {
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
                            bool negative = _in.get() > 0;
                            int64_t param = readUVarInt();
                            if (negative)
                                param = -param;
                            if (*c == 'c')
                                out.put(char(param));
                            else
                                out << param;
                            break;
                        }
                        case 'x': case 'X':
                            out << hex << readUVarInt() << dec;
                            break;
                        case 'u': {
                            out << readUVarInt();
                            break;
                        }
                        case 'e': case 'E':
                        case 'f': case 'F':
                        case 'g': case 'G':
                        case 'a': case 'A': {
                            fleece::endian::littleEndianDouble param;
                            _in.read((char*)&param, sizeof(param));
                            out << param;
                            break;
                        }
                        case '@':
                        case 's': {
                            if (minus && !dotStar) {
                                out << readStringToken();
                            } else {
                                size_t size = (size_t)readUVarInt();
                                char buf[200];
                                while (size > 0) {
                                    auto n = min(size, sizeof(buf));
                                    _in.read(buf, n);
                                    if (minus) {
                                        for (size_t i = 0; i < n; ++i) {
                                            char hex[3];
                                            sprintf(hex, "%02x", uint8_t(buf[i]));
                                            out << hex;
                                        }
                                    } else {
                                        out.write(buf, n);
                                    }
                                    size -= n;
                                }
                            }
                            break;
                        }
                        case 'p': {
                            out << "0x" << hex;
                            if (_pointerSize == 8) {
                                uint64_t ptr;
                                _in.read((char*)&ptr, sizeof(ptr));
                                out << ptr;
                            } else {
                                uint32_t ptr;
                                _in.read((char*)&ptr, sizeof(ptr));
                                out << ptr;
                            }
                            out << dec;
                            break;
                        }
                        case '%':
                            out << '%';
                            break;
                        default:
                            throw invalid_argument("Unknown type in LogDecoder format string");
                    }
                }
            }
        } catch (std::ios_base::failure &x) {
            reraise(x);
        }
    }


    const string& LogDecoder::readStringToken() {
        auto tokenID = (size_t)readUVarInt();
        if (tokenID < _tokens.size()) {
            return _tokens[tokenID];
        } else if (tokenID == _tokens.size()) {
            _tokens.push_back(readCString());
            return _tokens.back();
        } else {
            throw runtime_error("Invalid token string ID in log data");
        }
    }


    string LogDecoder::readCString() {
        string str;
        str.reserve(20);
        int c;
        while (0 < (c = _in.get()))
            str.push_back(char(c));
        if (c < 0)
            throw runtime_error("Unexpected EOF in log data");
        return str;
    }


    void LogDecoder::reraise(const std::ios_base::failure &x) {
        if (_in.good())
            throw x;                                // exception isn't on _in, so pass it on
        auto state = _in.rdstate();
        _in.clear();
        const char *message;
        if (state & ios_base::eofbit)
            message = "unexpected EOF in log";
        else if (state & ios_base::failbit)
            message = "error decoding log";
        else
            message = "I/O error reading log";
        char what[50];
        sprintf(what, "%s at %lld", message, (long long)_in.tellg());
        throw error(what);
    }


// Begin code extracted from varint.cc
    struct slice {
        const void *buf;
        size_t size;
    };

    enum {
        kMaxVarintLen16 = 3,
        kMaxVarintLen32 = 5,
        kMaxVarintLen64 = 10,
    };

    static size_t _GetUVarInt(slice buf, uint64_t *n) {
        // NOTE: The public inline function GetUVarInt already decodes 1-byte varints,
        // so if we get here we can assume the varint is at least 2 bytes.
        auto pos = (const uint8_t*)buf.buf;
        auto end = pos + std::min(buf.size, (size_t)kMaxVarintLen64);
        uint64_t result = *pos++ & 0x7F;
        int shift = 7;
        while (pos < end) {
            uint8_t byte = *pos++;
            if (byte >= 0x80) {
                result |= (uint64_t)(byte & 0x7F) << shift;
                shift += 7;
            } else {
                result |= (uint64_t)byte << shift;
                *n = result;
                size_t nBytes = pos - (const uint8_t*)buf.buf;
                if (nBytes == kMaxVarintLen64 && byte > 1)
                    nBytes = 0; // Numeric overflow
                return nBytes;
            }
        }
        return 0; // buffer too short
    }

    static inline size_t GetUVarInt(slice buf, uint64_t *n) {
        if (buf.size == 0)
            return 0;
        uint8_t byte = *(const uint8_t*)buf.buf;
        if (byte < 0x80) {
            *n = byte;
            return 1;
        }
        return _GetUVarInt(buf, n);
    }
// End code extracted from varint.cc


    uint64_t LogDecoder::readUVarInt() {
        uint8_t buf[10];
        for (int i = 0; i < 10; ++i) {
            int byte = _in.get();
            if (byte < 0)
                throw runtime_error("Unexpected EOF in log data");
            buf[i] = uint8_t(byte);
            if (byte < 0x80) {
                uint64_t n = 0;
                GetUVarInt(slice{&buf, size_t(i+1)}, &n);
                return n;
            }
        }
        throw runtime_error("Invalid varint encoding in log data");
    }

}
