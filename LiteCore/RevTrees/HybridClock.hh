//
// HybridClock.hh
//
// Copyright © 2023 Couchbase. All rights reserved.
//

#pragma once
#include "VersionTypes.hh"
#include <atomic>
#include <memory>

namespace litecore {

    /// The number of nanoseconds in a second.
    static constexpr uint64_t kNsPerSec(1e9);

    /** A local wall-clock time expressed as a 64-bit count of nanoseconds since the Unix epoch. */
    enum class walltime_t : uint64_t { epoch = 0 };

    /** Converts a wall-time to a number of seconds since the Unix epoch. */
    static inline double asSeconds(walltime_t t) { return double(t) / kNsPerSec; }

    /// Converts a logical timestamp to a number of seconds since the Unix epoch.
    /// This will not necessarily match the local time, even for a hybrid-time created locally;
    /// but it should at worst be slightly ahead. */
    static inline double asSeconds(logicalTime t) { return double(t) / kNsPerSec; }

    /// An object that provides the time, as a `walltime_t`, for a HybridClock.
    class ClockSource {
      public:
        virtual ~ClockSource()              = default;
        virtual walltime_t now()            = 0;  ///< Current time
        virtual walltime_t minValid() const = 0;  ///< Minimum `walltime_t` that could be a valid time
    };

    /// ClockSource that provides the real time from the system clock.
    class RealClockSource : public ClockSource {
      public:
        walltime_t now() override;
        walltime_t minValid() const override;
    };

    /// Fake ClockSource for tests that just increments a counter each time `now` is called.
    class FakeClockSource : public ClockSource {
      public:
        FakeClockSource(uint64_t t = 0, uint64_t step = 0x10000);
        walltime_t now() override;
        walltime_t minValid() const override;

        void setTime(uint64_t t) { _lastTime = t >> 16; }

      private:
        std::atomic<uint64_t> _lastTime = 0;
        uint64_t              _start;
        uint64_t              _step;
    };

    /** A "Hybrid Logical Clock" that tells time in `logicalTime` values by combining
        real (wall) time with a logical counter. It's based on the algorithms in the paper
        "Logical Physical Clocks and Consistent Snapshots in Globally Distributed Databases"
        (Kulkarni et al, 2014) <https://cse.buffalo.edu/tech-reports/2014-04.pdf>.
        \note  This class is thread-safe. */
    class HybridClock {
      public:
        /// The limit to how far ahead a received timestamp can be, in ns. (2 minutes).
        /// Beyond this, see() and seenTime() will throw an exception.
        static constexpr uint64_t kMaxClockSkew = 2 * 60 * kNsPerSec;

        /// Initializes a new instance.
        HybridClock(uint64_t state = 0);

        /// The current state, for storing persistently.
        /// \warning: This number is too large to convert to `double` without loss of accuracy,
        ///     which means storing it in JSON as a number may also lose accuracy, depending on the
        ///     JSON library. (Fleece can handle it.)
        [[nodiscard]] uint64_t state() const { return uint64_t(_lastTime.load()); }

        /// Returns a timestamp for the current moment. It is guaranteed to be larger than any
        /// previous one returned by `now`, or seen by `see` or `seenTime`.
        [[nodiscard]] logicalTime now();

        /// Updates internal state based on a timestamp received from elsewhere, to guarantee that
        /// the value of `now()` will be greater than this timestamp.
        /// @param t  A hybrid timestamp created elsewhere.
        /// @return  True on success, false if the timestamp is too far ahead of the local clock.
        /// \note  It's important to call this whenever a timestamp is received,
        ///         so local timestamps don't drift apart, and to ensure that
        ///         a newly created timestamp is greater than any existing timestamp.
        [[nodiscard]] bool see(logicalTime t);

        /// Registers a timestamp received from elsewhere, and returns a current timestamp
        /// corresponding to receiving that timestamp.
        [[nodiscard]] logicalTime seenTime(logicalTime);

        /// Returns true if the number is a valid timestamp.
        /// (It needs to be greater than about 2^60, but less than 2^63.)
        [[nodiscard]] bool validTime(logicalTime) const;

        /// For testing purposes only! Replaces the ClockSource so you can use a fake one.
        void setSource(std::unique_ptr<ClockSource>);

      private:
        template <typename FN>
        logicalTime        update(FN const&);
        logicalTime        seenTime(logicalTime, bool skip);
        [[nodiscard]] bool checkTime(logicalTime) const;

        std::unique_ptr<ClockSource> _source;
        logicalTime                  _minValid;
        std::atomic<logicalTime>     _lastTime = logicalTime::none;
    };

}  // namespace litecore
