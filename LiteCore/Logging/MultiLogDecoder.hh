//
// MultiLogDecoder.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "LogDecoder.hh"
#include "TextLogDecoder.hh"
#include <algorithm>
#include <array>
#include <climits>
#include <fstream>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include "betterassert.hh"

namespace litecore {

    /** Iterates over a set of logs, merging all their items in chronological order. */
    class MultiLogDecoder : public LogIterator {
      public:
        MultiLogDecoder() {
            _startTime = kMaxTimestamp;
            for ( unsigned i = 0; i <= kMaxLevel; i++ ) _startTimeByLevel[i] = kMaxTimestamp;
        }

        /// Adds a log iterator. Must be called before calling \ref next().
        /// The iterator is assumed to be at its start, so its \ref next() will be called first.
        void add(LogIterator* log) {
            assert(!_current);

            auto startTime = log->startTime();
            _startTime     = std::min(_startTime, startTime);
            if ( !log->next() ) return;
            _logs.push(log);
            if ( auto level = log->level(); level >= 0 && level <= kMaxLevel )
                _startTimeByLevel[level] = std::min(_startTimeByLevel[level], startTime);
        }

        // Adds a LogDecoder on the log file at the given path.
        bool add(const std::string& logPath) {
            std::ifstream in(logPath, std::ifstream::in | std::ifstream::binary);
            if ( !in ) return false;
            in.exceptions(std::ifstream::badbit);
            _inputs.push_back(std::move(in));
            std::unique_ptr<LogIterator> decoder;
            if ( TextLogDecoder::looksTextual(_inputs.back()) )
                decoder = std::make_unique<TextLogDecoder>(_inputs.back());
            else
                decoder = std::make_unique<LogDecoder>(_inputs.back());
            _decoders.push_back(std::move(decoder));
            add(_decoders.back().get());
            return true;
        }

        /// Time when the earliest log began
        [[nodiscard]] Timestamp startTime() const override { return _startTime; }

        /// Time that earliest logs at `level` begin, or kMaxTimestamp if none.
        [[nodiscard]] Timestamp startTimeOfLevel(unsigned level) const { return _startTimeByLevel[level]; }

        /// First time when logs of all levels are available
        [[nodiscard]] Timestamp fullStartTime() const {
            Timestamp fullStartTime = kMinTimestamp;
            for ( auto& ts : _startTimeByLevel ) {
                if ( fullStartTime < ts && ts != kMaxTimestamp ) fullStartTime = ts;
            }
            return fullStartTime;
        }

        bool next() override {
            if ( _current ) {
                assert(_current == _logs.top());
                _logs.pop();
                if ( _current->next() ) _logs.push(_current);
            }
            if ( _logs.empty() ) {
                _current = nullptr;
                return false;
            } else {
                _current = _logs.top();
                return true;
            }
        }

        [[nodiscard]] Timestamp timestamp() const override { return _current->timestamp(); }

        [[nodiscard]] int8_t level() const override { return _current->level(); }

        [[nodiscard]] const std::string& domain() const override { return _current->domain(); }

        [[nodiscard]] uint64_t objectID() const override { return _current->objectID(); }

        [[nodiscard]] const std::string* objectDescription() const override { return _current->objectDescription(); }

        std::string readMessage() override { return _current->readMessage(); }

        void decodeMessageTo(std::ostream& o) override { _current->decodeMessageTo(o); }

      private:
        static constexpr unsigned kMaxLevel = 4;

        struct logcmp {
            bool operator()(LogIterator* lhs, LogIterator* rhs) const {
                // priority_queue sorts in descending order, so compare using '>'.
                // It requires a strict ordering (two items can never be equal), so we break
                // ties arbitrarily by comparing the iterator pointers.
                if ( rhs->timestamp() < lhs->timestamp() ) return true;
                else if ( rhs->timestamp() == lhs->timestamp() )
                    return intptr_t(rhs) < intptr_t(lhs);
                else
                    return false;
            }
        };

        std::priority_queue<LogIterator*, std::vector<LogIterator*>, logcmp> _logs;
        LogIterator*                                                         _current{nullptr};
        Timestamp                                                            _startTime{};
        std::array<Timestamp, kMaxLevel + 1>                                 _startTimeByLevel{};

        std::vector<std::unique_ptr<LogIterator>> _decoders;
        std::deque<std::ifstream>                 _inputs;
    };


}  // namespace litecore
