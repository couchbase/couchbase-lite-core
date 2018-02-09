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
#include "LogEncoder.hh"
#include "Endian.hh"
#include "varint.hh"
#include "PlatformCompat.hh"
#include <exception>
#include <sstream>
#include <time.h>

#if __APPLE__
#include <sys/time.h>
#endif

using namespace std;
using namespace fleece;

namespace litecore {

    // The units we count in are microseconds.
    static constexpr unsigned kTicksPerSec = 1000000;

    LogDecoder::LogDecoder(std::istream &in)
    :_in(in)
    {
        _in.exceptions(istream::badbit | istream::failbit | istream::eofbit);
        uint8_t header[6];
        _in.read((char*)&header, sizeof(header));
        if (memcmp(&header, &LogEncoder::kMagicNumber, 4) != 0)
            throw runtime_error("Not a LiteCore log file");
        if (header[4] != LogEncoder::kFormatVersion)
            throw runtime_error("Unsupported log format version");
        _pointerSize = header[5];
        if (_pointerSize != 4 && _pointerSize != 8)
            throw runtime_error("This log file seems to be damaged");
        _startTime = time_t(readUVarInt());
        _readMessage = true;
    }


    bool LogDecoder::next() {
        if (!_readMessage)
            readMessage();
        
        _in.exceptions(istream::badbit | istream::failbit);  // turn off EOF exception temporarily
        if (!_in || _in.peek() < 0)
            return false;
        _in.exceptions(istream::badbit | istream::failbit | istream::eofbit);

        _elapsedTicks += readUVarInt();
        _curLevel = (int8_t)_in.get();
        _curDomain = &readStringToken();
        _readMessage = false;
        return true;
    }


    void LogDecoder::decodeTo(ostream &out, const std::vector<std::string> &levelNames) {
        writeTimestamp({_startTime, 0}, out);
        struct tm tm;
        localtime_r(&_startTime, &tm);
        char datestamp[100];
        strftime(datestamp, sizeof(datestamp), "---- Logging begins on %A, %x ----\n", &tm);
        out << datestamp;

        while (next()) {
            writeTimestamp(timestamp(), out);

            string levelName;
            if (_curLevel >= 0 && _curLevel < levelNames.size())
                levelName = levelNames[_curLevel];
            writeHeader(levelName, *_curDomain, out);
            decodeMessageTo(out);
            out << '\n';
        }
    }


    /*static*/ LogDecoder::Timestamp LogDecoder::now() {
        using namespace chrono;

        auto now = time_point_cast<microseconds>(system_clock::now());
        auto count = now.time_since_epoch().count();
        time_t secs = (time_t)count / 1000000;
        unsigned microsecs = count % 1000000;
        return {secs, microsecs};
    }


    /*static*/ void LogDecoder::writeHeader(const string &levelName,
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


    LogDecoder::Timestamp LogDecoder::timestamp() const {
        return {_startTime + time_t(_elapsedTicks / kTicksPerSec),
                uint32_t(_elapsedTicks % kTicksPerSec)};
    }


    string LogDecoder::readMessage() {
        stringstream out;
        decodeMessageTo(out);
        return out.str();
    }


#pragma mark - INNARDS:


    void LogDecoder::writeTimestamp(Timestamp t, ostream &out) {
        struct tm tm;
        localtime_r(&t.secs, &tm);
        char timestamp[100];
        strftime(timestamp, sizeof(timestamp), "%T", &tm);
        out << timestamp;
        sprintf(timestamp, ".%06u| ", t.microsecs);
        out << timestamp;
    }


    void LogDecoder::decodeMessageTo(ostream &out) {
        assert(!_readMessage);
        _readMessage = true;

        // Read the object ID:
        uint64_t objRef = readUVarInt();
        if (objRef > 0) {
            if (_objects.find(objRef) != _objects.end()) {
                out << '{' << objRef << "} ";
            } else {
                string description = readCString();
                _objects.insert({objRef, description});
                out << '{' << objRef << "|" << description << "} ";
            }
        }

        // Read the format string, then the parameters:
        const char *format = readStringToken().c_str();
        for (const char *c = format; *c != '\0'; ++c) {
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
                        littleEndianDouble param;
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


    uint64_t LogDecoder::readUVarInt() {
        uint8_t buf[10];
        for (int i = 0; i < 10; ++i) {
            int byte = _in.get();
            if (byte < 0)
                throw runtime_error("Unexpected EOF in log data");
            buf[i] = uint8_t(byte);
            if (byte < 0x80) {
                uint64_t n;
                GetUVarInt(slice(&buf, i+1), &n);
                return n;
            }
        }
        throw runtime_error("Invalid varint encoding in log data");
    }

}
