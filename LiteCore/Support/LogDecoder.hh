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
#include <ios>
#include <map>
#include <optional>
#include <stdint.h>
#include <string>
#include <vector>

namespace litecore {

    /** Abstract interface for reading logs. */
    class LogIterator {
    public:
        /** A timestamp, given as a standard time_t (seconds since 1/1/1970) plus a number of
         microseconds. */
        struct Timestamp {
            time_t secs;
            unsigned microsecs;
        };

        virtual ~LogIterator() = default;

        /** Decodes the entire log and writes it to the output stream, with timestamps.
            If you want more control over the presentation, use the other methods below to
            read the timestamps and messages individually. */
        virtual void decodeTo(std::ostream&,
                              const std::vector<std::string> &levelNames,
                              std::optional<Timestamp> startingAt =std::nullopt);

        /** Reads the next line from the log, or returns false at EOF. */
        virtual bool next() =0;

        /** Returns the time logging began. */
        virtual Timestamp startTime() const =0;

        /** Returns the current line's timestamp. */
        virtual Timestamp timestamp() const =0;

        /** Returns the current line's level. */
        virtual int8_t level() const =0;

        /** Returns the current line's domain. */
        virtual const std::string& domain() const =0;

        virtual uint64_t objectID() const =0;
        virtual const std::string* objectDescription() const =0;

        /** Reads the next message from the input and returns it as a string.
         You can only read each message once; calling this twice in a row will fail. */
        virtual std::string readMessage();

        /** Reads the next message from the input and writes it to the output.
         You can only read each message once; calling this twice in a row will fail.  */
        virtual void decodeMessageTo(std::ostream&) =0;

        static Timestamp now();
        static std::string formatDate(Timestamp);
        static void writeISO8601DateTime(Timestamp, std::ostream&);
        static void writeTimestamp(Timestamp, std::ostream&);
        static void writeHeader(const std::string &levelName,
                                const std::string &domainName,
                                std::ostream&);
    };


    /** Decodes logs written by LogEncoder. */
    class LogDecoder : public LogIterator {
    public:
        static const uint8_t kMagicNumber[4];

        /** Initializes decoder with a stream written by a LogEncoder. */
        LogDecoder(std::istream&);

        // LogIterator API:
        void decodeTo(std::ostream&,
                      const std::vector<std::string> &levelNames,
                      std::optional<Timestamp> startingAt =std::nullopt) override;
        bool next() override;
        Timestamp startTime() const override             {return {_startTime, 0};}
        Timestamp timestamp() const override             {return _timestamp;}
        int8_t level() const override                    {return _curLevel;}
        const std::string& domain() const override       {return *_curDomain;}
        uint64_t objectID() const override;
        const std::string* objectDescription() const override;
        void decodeMessageTo(std::ostream&) override;

        static constexpr uint8_t kFormatVersion = 1;

        /** Exception thrown from \ref next, \ref readMessage, or \ref decodeMessageTo
            if an I/O error or unexpected EOF occurs on the input stream. */
        class error : public std::runtime_error {
        public:
            explicit error(const char *msg)         :runtime_error(msg) { }
        };

    private:
        uint64_t readUVarInt();
        const std::string& readStringToken();
        std::string readCString();
        [[noreturn]] void reraise(const std::ios_base::failure&);

        std::istream &_in;
        size_t _pointerSize;
        time_t _startTime;
        uint64_t _elapsedTicks {0};
        Timestamp _timestamp;
        std::vector<std::string> _tokens;
        std::map<uint64_t,std::string> _objects;

        int8_t _curLevel {0};
        const std::string *_curDomain {nullptr};
        uint64_t _curObject;
        bool _curObjectIsNew;
        mutable bool _putCurObjectInMessage;
        bool _readMessage;
    };


    static inline bool operator==(const LogDecoder::Timestamp &a, const LogDecoder::Timestamp &b) {
        return a.secs == b.secs && a.microsecs == b.microsecs;
    }

    static inline bool operator< (const LogDecoder::Timestamp &a, const LogDecoder::Timestamp &b) {
        return a.secs < b.secs || (a.secs == b.secs && a.microsecs < b.microsecs);
    }

    static inline std::ostream& operator<< (std::ostream &out, const LogDecoder::Timestamp &ts) {
        LogDecoder::writeTimestamp(ts, out);
        return out;
    }

}
