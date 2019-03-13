//
// LogDecoder.hh
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

#pragma once
#include <ctime>
#include <iostream>
#include <map>
#include <stdint.h>
#include <string>
#include <vector>

namespace litecore {

    /** Decodes logs written by LogEncoder. */
    class LogDecoder {
    public:
        static const uint8_t kMagicNumber[4];

        /** Initializes decoder with a stream written by a LogEncoder. */
        LogDecoder(std::istream&);

        /** Decodes the entire log and writes it to the output stream, with timestamps.
            If you want more control over the presentation, use the other methods below to
            read the timestamps and messages individually. */
        void decodeTo(std::ostream&, const std::vector<std::string> &levelNames);

        /** Reads the next line from the log, or returns false at EOF. */
        bool next();

        /** A timestamp, given as a standard time_t (seconds since 1/1/1970) plus a number of
            microseconds. */
        struct Timestamp {
            time_t secs;
            unsigned microsecs;
        };

        /** Returns the current line's timestamp. */
        Timestamp timestamp() const;

        /** Returns the current line's level. */
        int8_t level() const                    {return _curLevel;}

        /** Returns the current line's domain. */
        const std::string& domain() const       {return *_curDomain;}

        uint64_t objectID() const;
        const std::string* objectDescription() const;

        /** Reads the next message from the input and returns it as a string.
            You can only read each message once; calling this twice in a row will fail. */
        std::string readMessage();

        /** Reads the next message from the input and writes it to the output.
            You can only read each message once; calling this twice in a row will fail.  */
        void decodeMessageTo(std::ostream&);

        static Timestamp now();
        static void writeTimestamp(Timestamp, std::ostream&);
        static void writeHeader(const std::string &levelName,
                                const std::string &domainName,
                                std::ostream&);

        static constexpr uint8_t kFormatVersion = 1;

    private:
        uint64_t readUVarInt();
        const std::string& readStringToken();
        std::string readCString();

        std::istream &_in;
        size_t _pointerSize;
        time_t _startTime;
        uint64_t _elapsedTicks {0};
        std::vector<std::string> _tokens;
        std::map<uint64_t,std::string> _objects;

        int8_t _curLevel {0};
        const std::string *_curDomain {nullptr};
        uint64_t _curObject;
        bool _curObjectIsNew;
        bool _putCurObjectInMessage;
        bool _readMessage;
    };

}
