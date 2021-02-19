//
// MultiLogDecoder.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "LogDecoder.hh"
#include <algorithm>
#include <climits>
#include <fstream>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace litecore {

    /** Iterates over a set of logs, merging all their items in chronological order. */
    class MultiLogDecoder : public LogIterator {
    public:
        MultiLogDecoder() {
            _startTime = {UINT_MAX, 0};
            for (unsigned i = 0; i <= kMaxLevel; i++)
                _startTimeByLevel[i] = {UINT_MAX, 0};
        }

        /// Adds a log iterator. Must be called before calling \ref next().
        /// The iterator is assumed to be at its start, so its \ref next() will be called first.
        void add(LogIterator* log) {
            assert(!_current);
            if (!log->next())
                return;

            _logs.push(log);

            auto startTime = log->startTime();
            _startTime = std::min(_startTime, startTime);
            auto level = log->level();
            if (level >= 0 && level <= kMaxLevel)
                _startTimeByLevel[level] = std::min(_startTimeByLevel[level], startTime);
        }

        // Adds a LogDecoder on the log file at the given path.
        bool add(const std::string &logPath) {
            std::ifstream in(logPath, std::ifstream::in | std::ifstream::binary);
            if (!in)
                return false;
            in.exceptions(std::ifstream::badbit);
            _inputs.push_back(std::move(in));
            LogDecoder decoder(_inputs.back());
            _decoders.push_back(std::move(decoder));
            add(&_decoders.back());
            return true;
        }

        /// Time when the earliest log began
        Timestamp startTime() const override                 {return _startTime;}

        /// First time when logs of all levels are available
        Timestamp fullStartTime() const {
            Timestamp fullStartTime = {0, 0};
            for (unsigned i = 0; i <= kMaxLevel; i++)
                fullStartTime = std::max(fullStartTime, _startTimeByLevel[i]);
            return fullStartTime;
        }

        bool next() override {
            if (_current) {
                assert(_current == _logs.top());
                _logs.pop();
                if (_current->next())
                    _logs.push(_current);
            }
            if (_logs.empty()) {
                _current = nullptr;
                return false;
            } else {
                _current = _logs.top();
                return true;
            }
        }

        Timestamp timestamp() const override                 {return _current->timestamp();}
        int8_t level() const override                        {return _current->level();}
        const std::string& domain() const override           {return _current->domain();}
        uint64_t objectID() const override                   {return _current->objectID();}
        const std::string* objectDescription() const override{return _current->objectDescription();}
        std::string readMessage() override                   {return _current->readMessage();}
        void decodeMessageTo(std::ostream& o) override       {_current->decodeMessageTo(o);}

    private:
        static constexpr unsigned kMaxLevel = 4;

        struct logcmp {
            bool operator()(LogIterator *lhs, LogIterator *rhs) const {
                // priority_queue sorts in descending order, so compare using '>'.
                // It requires a strict ordering (two items can never be equal), so we break
                // ties arbitrarily by comparing the iterator pointers.
                if (rhs->timestamp() < lhs->timestamp())
                    return true;
                else if (rhs->timestamp() == lhs->timestamp())
                    return intptr_t(rhs) < intptr_t(lhs);
                else
                    return false;
            }
        };

        std::priority_queue<LogIterator*,std::vector<LogIterator*>,logcmp> _logs;
        LogIterator* _current {nullptr};
        Timestamp _startTime;
        Timestamp _startTimeByLevel[kMaxLevel+1];

        std::deque<LogDecoder> _decoders;
        std::deque<std::ifstream> _inputs;
    };


}
