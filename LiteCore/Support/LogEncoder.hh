//
// LogEncoder.hh
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
#include "Writer.hh"
#include "Stopwatch.hh"
#include "Timer.hh"
#include "PlatformCompat.hh"
#include "Logging.hh"
#include <stdarg.h>
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
        LogEncoder(std::ostream &out, LogLevel level);
        ~LogEncoder();

        enum ObjectRef : unsigned {
            None = 0
        };
        
        void vlog(const char *domain, const std::map<unsigned, std::string>&, ObjectRef, const char *format, va_list args);

        void log(const char *domain, const std::map<unsigned, std::string>&, ObjectRef, const char *format, ...) __printflike(5, 6);

        void flush();
        
        uint64_t tellp();

        /** A timestamp, given as a standard time_t (seconds since 1/1/1970) plus microseconds. */
        struct Timestamp {
            time_t secs;
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
        int64_t _timeElapsed() const;
        void _writeUVarInt(uint64_t);
        void _writeStringToken(const char *token);
        void _flush();
        void _scheduleFlush();
        void performScheduledFlush();

        std::mutex _mutex;
        fleece::Writer _writer;
        std::ostream &_out;
        std::unique_ptr<actor::Timer> _flushTimer;
        fleece::Stopwatch _st;
        int64_t _lastElapsed {0};
        int64_t _lastSaved {0};
        LogLevel _level;
        std::unordered_map<size_t, unsigned> _formats;
        std::unordered_set<unsigned> _seenObjects;
    };

}
