//
// LogEncoder.hh
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
#include "Writer.hh"
#include "Stopwatch.hh"
#include <cstdarg>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace litecore {
    namespace actor {
        class Timer;
    }

    /** A very fast & compact logging service.
        The output is written in a binary format to avoid the CPU and space overhead of converting
        everything to ASCII. It can be decoded by the LogDecoder class.
        The API is thread-safe. */
    class LogEncoder {
      public:
        /// Log level associated with this file. In practice, same as the `LogLevel` enum in Logging.hh.
        using LogLevel = int8_t;

        /// Constructor.
        /// @param out  The stream to write to.
        /// @param level  The log level associated with this encoder.
        LogEncoder(std::ostream& out, LogLevel level);
        ~LogEncoder();

        /// A unique identifier of an application object that can write log messages.
        enum ObjectRef : unsigned { None = 0 };

        /// Lowest-level method to write a log message.
        /// @param domain  The logging domain, e.g. "DB" or "Sync"
        /// @param obj  The ID of the object logging this message, else `None`.
        /// @param objPath  Metadata about the object. Will be written only the first time this object logs;
        ///                 otherwise it can safely be left empty. (Call `isNewObject` to check.)
        /// @param prefix  A prefix for the message.
        /// @param format  The printf-style format string. MUST be a string literal!
        /// @param args  The args corresponding to the format string.
        void vlog(const char* domain, ObjectRef obj, std::string_view objPath, const std::string& prefix,
                  const char* format, va_list args) __printflike(6, 0);

        /// Writes a log message.
        /// @param domain  The logging domain, e.g. "DB" or "Sync"
        /// @param obj  The ID of the object logging this message, else `None`.
        /// @param objPath  Metadata about the object. Will be written only the first time this object logs;
        ///                 otherwise it can safely be left empty. (Call `isNewObject` to check.)
        /// @param format  The printf-style format string. MUST be a string literal!
        void log(const char* domain, ObjectRef obj, std::string_view objPath, const char* format, ...)
                __printflike(5, 6);

        /// Writes a log message without an object.
        /// @param domain  The logging domain, e.g. "DB" or "Sync"
        /// @param format  The printf-style format string. MUST be a string literal!
        void log(const char* domain, const char* format, ...) __printflike(3, 4);

        /// Flushes any pending writes to the log stream.
        void flush();

        /// The current offset in the log stream.
        uint64_t tellp();

        /// Returns true if this ObjectRef has not yet logged.
        bool isNewObject(ObjectRef) const;

        /// A timestamp, given as a standard time_t (seconds since 1/1/1970) plus microseconds.
        struct Timestamp {
            time_t   secs;
            unsigned microsecs;
        };

        /// A way to interact with the output stream safely (since the encoder may be writing to
        /// it on a background thread.) The lambda should take a `std::ostream&` parameter.
        template <class LAMBDA>
        void withStream(LAMBDA with) {
            std::lock_guard<std::mutex> lock(_mutex);
            with(_out);
        }

      private:
        class Formats {
          public:
            unsigned find(const std::string& prefix, size_t fmt) {
                auto it = _map.find(prefix);
                if ( it == _map.end() ) { return end(); }
                auto innerIt = it->second.find(fmt);
                if ( innerIt == it->second.end() ) { return end(); }
                return innerIt->second;
            }

            // Pre-condition: find(prefix, fmt) == end()
            unsigned insert(const std::string& prefix, size_t fmt) {
                unsigned ret = _count;
                _map[prefix].emplace(fmt, _count++);
                return ret;
            }

            unsigned end() const { return _count; }

            unsigned size() const { return _count; }

          private:
            // Invariants: _count == number of _map[x][y] && _map[x][y] represents the order (x, y)
            //             is inserted, starting from 0.
            unsigned                                                              _count = 0;
            std::unordered_map<std::string, std::unordered_map<size_t, unsigned>> _map;
        };

        [[nodiscard]] int64_t _timeElapsed() const;
        void                  _writeUVarInt(uint64_t);
        void                  _writeStringToken(const char* token, const std::string& prefix = "");
        void                  _flush();
        void                  _scheduleFlush();
        void                  performScheduledFlush();

        mutable std::mutex            _mutex;           // Ensures thread-safety
        fleece::Writer                _writer;          // Lightweight output buffer
        std::ostream&                 _out;             // Heavyweight output stream (usually a file)
        std::unique_ptr<actor::Timer> _flushTimer;      // Timer for automatic calls to flush()
        fleece::Stopwatch             _st;              // Tracks time elapsed since start, for writing timestamps
        int64_t                       _lastElapsed{0};  // Timestamp of last message written
        int64_t                       _lastSaved{0};    // Timestamp of last flush
        LogLevel                      _level;           // The log level of this logger
        Formats                       _formats;         // Maps strings to integer tokens
        std::unordered_set<unsigned>  _seenObjects;     // Tracks which ObjectRefs have been written
    };

}  // namespace litecore
