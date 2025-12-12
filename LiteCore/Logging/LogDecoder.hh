//
// LogDecoder.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include <ctime>
#include <ios>
#include <map>
#include <optional>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace litecore {

    /** Abstract interface for reading logs. */
    class LogIterator {
      public:
        /** A timestamp, given as a standard time_t (seconds since 1/1/1970) plus a number of
         microseconds. */
        struct Timestamp {
            time_t   secs;
            unsigned microsecs;
        };

        static constexpr Timestamp kMinTimestamp{0, 0};
        static constexpr Timestamp kMaxTimestamp{std::numeric_limits<time_t>::max(), 999999};

        virtual ~LogIterator() = default;

        /** Decodes the entire log and writes it to the output stream, with timestamps.
            If you want more control over the presentation, use the other methods below to
            read the timestamps and messages individually. */
        virtual void decodeTo(std::ostream&, const std::vector<std::string>& levelNames,
                              std::optional<Timestamp> startingAt = std::nullopt);

        /** Reads the next line from the log, or returns false at EOF. */
        virtual bool next() = 0;

        /** Returns the time logging began. */
        [[nodiscard]] virtual Timestamp startTime() const = 0;

        /** Returns the current line's timestamp. */
        [[nodiscard]] virtual Timestamp timestamp() const = 0;

        /** Returns the current line's level. */
        [[nodiscard]] virtual int8_t level() const = 0;

        /** Returns the current line's domain. */
        [[nodiscard]] virtual const std::string& domain() const = 0;

        [[nodiscard]] virtual uint64_t           objectID() const          = 0;
        [[nodiscard]] virtual const std::string* objectDescription() const = 0;

        /** Reads the next message from the input and returns it as a string.
         You can only read each message once; calling this twice in a row will fail. */
        virtual std::string readMessage();

        /** Reads the next message from the input and writes it to the output.
         You can only read each message once; calling this twice in a row will fail.  */
        virtual void decodeMessageTo(std::ostream&) = 0;

        static Timestamp   now();
        static std::string formatDate(Timestamp);
        static void        writeISO8601DateTime(Timestamp, std::ostream&);
        static void        writeTimestamp(Timestamp, std::ostream&, bool inUtcTime = false);
        static void        writeHeader(const std::string& levelName, const std::string& domainName, std::ostream&);
    };

    /** Decodes logs written by LogEncoder. */
    class LogDecoder : public LogIterator {
      public:
        static const uint8_t kMagicNumber[4];

        /** Initializes decoder with a stream written by a LogEncoder. */
        explicit LogDecoder(std::istream&);

        // LogIterator API:
        void decodeTo(std::ostream&, const std::vector<std::string>& levelNames,
                      std::optional<Timestamp> startingAt = std::nullopt) override;
        bool next() override;

        Timestamp startTime() const override { return {_startTime, 0}; }

        Timestamp timestamp() const override { return _timestamp; }

        int8_t level() const override { return _curLevel; }

        const std::string& domain() const override { return *_curDomain; }

        uint64_t           objectID() const override;
        const std::string* objectDescription() const override;
        void               decodeMessageTo(std::ostream&) override;

        static constexpr uint8_t kFormatVersion = 1;

        /** Exception thrown from \ref next, \ref readMessage, or \ref decodeMessageTo
            if an I/O error or unexpected EOF occurs on the input stream. */
        class error : public std::runtime_error {
          public:
            explicit error(const char* msg) : runtime_error(msg) {}
        };

      private:
        uint64_t           readUVarInt();
        const std::string& readStringToken();
        std::string        readCString();
        [[noreturn]] void  reraise(const std::ios_base::failure&);

        std::istream&                   _in;
        size_t                          _pointerSize;
        time_t                          _startTime;
        uint64_t                        _elapsedTicks{0};
        Timestamp                       _timestamp{};
        std::vector<std::string>        _tokens;
        std::map<uint64_t, std::string> _objects;

        int8_t             _curLevel{0};
        const std::string* _curDomain{nullptr};
        uint64_t           _curObject{};
        bool               _curObjectIsNew{};
        mutable bool       _putCurObjectInMessage{};
        bool               _readMessage;
    };

    inline bool operator==(const LogDecoder::Timestamp& a, const LogDecoder::Timestamp& b) {
        return a.secs == b.secs && a.microsecs == b.microsecs;
    }

    inline bool operator<(const LogDecoder::Timestamp& a, const LogDecoder::Timestamp& b) {
        return a.secs < b.secs || (a.secs == b.secs && a.microsecs < b.microsecs);
    }

    inline std::ostream& operator<<(std::ostream& out, const LogDecoder::Timestamp& ts) {
        LogDecoder::writeTimestamp(ts, out);
        return out;
    }

}  // namespace litecore
