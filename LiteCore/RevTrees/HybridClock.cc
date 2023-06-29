//
// HybridClock.cc
//
// Copyright © 2023 Couchbase. All rights reserved.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "HybridClock.hh"
#include "Error.hh"
#include "Logging.hh"
#include "slice_stream.hh"
#include <time.h>

#ifdef _MSC_VER
#    include <Windows.h>
#endif

namespace litecore {
    using namespace std;

    // An arbitrary timestamp that's the lowest we're willing to accept -- 27 June 2023, noonish.
    static constexpr walltime_t kMinValidTime{0x176c9a6fd6900000};

    // Highest timestamp that could ever be valid; this is sometime in the year 2262.
    // This is a sanity check to detect obviously bogus values like negative numbers.
    static constexpr logicalTime kMaxValidTime{0x7fffffffffffffff};

    walltime_t RealClockSource::now() {
#ifdef _MSC_VER
        // https://stackoverflow.com/posts/51974214/revisions

        static constexpr uint64_t MS_PER_SEC  = 1000ULL;  // MS = milliseconds
        static constexpr uint64_t US_PER_MS   = 1000ULL;  // US = microseconds
        static constexpr uint64_t HNS_PER_US  = 10ULL;    // HNS = hundred-nanoseconds
        static constexpr uint64_t HNS_PER_SEC = (MS_PER_SEC * US_PER_MS * HNS_PER_US);
        static constexpr uint64_t NS_PER_HNS  = (100ULL);  // NS = nanoseconds

        FILETIME       ft;
        ULARGE_INTEGER hnsTime;

        GetSystemTimePreciseAsFileTime(&ft);

        hnsTime.LowPart  = ft.dwLowDateTime;
        hnsTime.HighPart = ft.dwHighDateTime;

        // To get POSIX Epoch as baseline,
        // subtract the number of hns intervals from Jan 1, 1601 to Jan 1, 1970.
        hnsTime.QuadPart -= (11644473600ULL * HNS_PER_SEC);
        return walltime_t{hnsTime.QuadPart * NS_PER_HNS};
#else
        timespec ts;
        if ( ::clock_gettime(CLOCK_REALTIME, &ts) != 0 )
            throw std::runtime_error("Can't get current time; clock_gettime failed");
        return walltime_t{ts.tv_sec * kNsPerSec + ts.tv_nsec};
#endif
    }

    walltime_t RealClockSource::minValid() const { return kMinValidTime; }

    FakeClockSource::FakeClockSource(uint64_t t, uint64_t step) : _lastTime(t >> 16), _start(t), _step(step) {}

    walltime_t FakeClockSource::now() { return walltime_t{_lastTime += _step}; }

    walltime_t FakeClockSource::minValid() const { return walltime_t{_start + _step}; }

    /// A logicalTime broken into its walltime_t and counter components.
    struct hybridComponents {
        walltime_t wall;
        uint16_t   counter;

        hybridComponents() : wall{}, counter{} {}

        hybridComponents(walltime_t w, uint16_t c) : wall(walltime_t{uint64_t(w) & ~0xFFFFull}), counter(c) {}

        explicit hybridComponents(logicalTime t)
            : wall(walltime_t{uint64_t(t) & ~0xFFFFull}), counter(uint16_t(uint64_t(t) & 0xFFFF)) {}

        operator logicalTime() const { return logicalTime{(uint64_t(wall) & ~0xFFFF) | counter}; }
    };

    HybridClock::HybridClock(uint64_t state)
        : _source(std::make_unique<RealClockSource>())
        , _lastTime(logicalTime{state})
        , _minValid(logicalTime(_source->minValid())) {}

    void HybridClock::setSource(std::unique_ptr<ClockSource> src) {
        _source   = std::move(src);
        _minValid = logicalTime(_source->minValid());
        _lastTime = logicalTime::none;
    }

    template <typename FN>
    logicalTime HybridClock::update(FN const& fn) {
        logicalTime now;
        logicalTime then = _lastTime.load();
        do {
            now = logicalTime(fn(hybridComponents(then)));
            if ( now == logicalTime::none ) break;
        } while ( !_lastTime.compare_exchange_strong(then, now) );
        return now;
    }

    // These methods implement the HLC algorithm in figure 5 of the paper.

    logicalTime HybridClock::now() {
        return update([&](hybridComponents then) {
            hybridComponents now(_source->now(), 0);
            if ( now.wall <= then.wall ) {
                now = then;
                now.counter++;
            }
            return now;
        });
    }

    bool HybridClock::see(logicalTime seen) {
        if ( !checkTime(seen) ) return false;
        else if ( seen <= _lastTime.load() )
            return true;
        else
            return seenTime(seen, false) != logicalTime::none;
    }

    logicalTime HybridClock::seenTime(logicalTime seenT) {
        if ( !checkTime(seenT) ) return logicalTime::none;
        return seenTime(seenT, true);
    }

    logicalTime HybridClock::seenTime(logicalTime seenT, bool skipPast) {
        hybridComponents seen(seenT);
        return update([&](hybridComponents then) {
            auto localWall = _source->now();
            if ( uint64_t(seen.wall) > uint64_t(localWall) + kMaxClockSkew ) {
                Warn("HybridClock: received time 0x%016llx is too far in the future (local time is "
                     "0x%016llx)",
                     uint64_t(seen.wall), uint64_t(localWall));
                return hybridComponents{};
            }

            hybridComponents now(std::max(seen.wall, std::max(then.wall, localWall)), 0);
            if ( now.wall == then.wall ) {
                if ( now.wall == seen.wall ) now.counter = std::max(then.counter, seen.counter) + skipPast;
                else
                    now.counter = then.counter + skipPast;
            } else if ( now.wall == seen.wall ) {
                now.counter = seen.counter + skipPast;
            }
            return now;
        });
    }

    bool HybridClock::checkTime(logicalTime t) const {
        if ( t < _minValid ) {
            Warn("HybridClock: received time 0x%016llx is too far in the past", uint64_t(t));
            return false;
        } else if ( t > kMaxValidTime ) {
            Warn("HybridClock: received time 0x%016llx is invalid; distant future", uint64_t(t));
            return false;
        } else {
            return true;
        }
    }

    bool HybridClock::validTime(logicalTime t) const { return t >= _minValid && t <= kMaxValidTime; }

}  // namespace litecore
