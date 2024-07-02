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
#include "Timer.hh"
#include "Logging.hh"
#include <cstdarg>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace litecore {

    /** A very fast & compact logging service.
        The output is written in a binary format to avoid the CPU and space overhead of converting
        everything to ASCII. It can be decoded by the LogDecoder class.
        The API is thread-safe. */
    class LogEncoder {
      public:
        LogEncoder(std::ostream& out, LogLevel level);
        ~LogEncoder();

        enum ObjectRef : unsigned { None = 0 };

        void vlog(const char* domain, const LogDomain::ObjectMap&, ObjectRef, const std::string& prefix,
                  const char* format, va_list args) __printflike(6, 0);

        void log(const char* domain, const LogDomain::ObjectMap&, ObjectRef, const char* format, ...)
                __printflike(5, 6);

        void flush();

        uint64_t tellp();

        /** A timestamp, given as a standard time_t (seconds since 1/1/1970) plus microseconds. */
        struct Timestamp {
            time_t   secs;
            unsigned microsecs;
        };

        /** A way to interact with the output stream safely (since the encoder may be writing to
            it on a background thread.) */
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

        std::mutex                    _mutex;
        fleece::Writer                _writer;
        std::ostream&                 _out;
        std::unique_ptr<actor::Timer> _flushTimer;
        fleece::Stopwatch             _st;
        int64_t                       _lastElapsed{0};
        int64_t                       _lastSaved{0};
        LogLevel                      _level;
        Formats                       _formats;
        std::unordered_set<unsigned>  _seenObjects;
    };

}  // namespace litecore
