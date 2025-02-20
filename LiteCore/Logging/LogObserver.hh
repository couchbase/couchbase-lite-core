//
// LogObserver.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Logging.hh"
#include "fleece/RefCounted.hh"
#include "fleece/slice.hh"
#include <cstdarg>
#include <span>
#include <string>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    struct RawLogEntry;

    /** Struct representing a log message and its metadata. */
    struct LogEntry {
        LogEntry(uint64_t t, LogDomain& d, LogLevel lv, fleece::slice msg);
        LogEntry(RawLogEntry const&, const char* format, va_list) __printflike(3, 0);

        uint64_t const            timestamp;  ///< Time the event occurred, in ms since Unix epoch (same as C4Timestamp)
        LogDomain&                domain;     ///< Domain being logged to
        LogLevel const            level;      ///< Severity level
        fleece::alloc_slice const message;    ///< The text. Guaranteed to be followed by a 00 byte.

        const char* messageStr() const { return static_cast<const char*>(message.buf); }
    };

    /** Struct representing the metadata of an unformatted log message.
        The message itself is not included; it's passed as separate `fmt` and `args` parameters. */
    struct RawLogEntry {
        uint64_t const     timestamp;  ///< Time the event occurred
        LogDomain&         domain;     ///< Domain being logged to
        LogLevel const     level;      ///< Severity level
        LogObjectRef const objRef;     ///< Registered object that logged the mesage, else None (0)
        const std::string& prefix;     ///< Optional prefix string to add to the message
        bool const         fileOnly;   ///< If true, _only_ LogFiles should log this
    };

    /** Abstract class that receives log messages as they're written. */
    class LogObserver : public fleece::RefCounted {
      public:
        /// Registers a LogObserver for the specified set of log domains and minimum levels.
        /// For domains not in the list, it will use `defaultLevel`.
        /// @throws std::invalid_argument  if the observer is already registered.
        static void add(LogObserver*, LogLevel defaultLevel, std::span<const std::pair<LogDomain&, LogLevel>> = {});

        /// Unregisters a LogObserver.
        static void remove(LogObserver*);

        //---- Instance API:

        bool raw() const { return _raw; }

        /// Informs a LogObserver of a new log message. Only called if `raw` is false.
        /// @warning  This method may be called concurrently. Implementation is responsible for thread-safety.
        virtual void observe(LogEntry const&) noexcept;

        /// Informs a LogObserver of a new log message. Only called if `raw` is true.
        /// @warning  This method may be called concurrently. Implementation is responsible for thread-safety.
        virtual void observe(RawLogEntry const&, const char* format, va_list args) noexcept;

      protected:
        /// @param raw  True if you want a RawLogEntry, false for the regular parsed LogEntry.
        explicit LogObserver(bool raw = false) : _raw(raw) {}

      private:
        friend class LogCallback;
        friend class LogFiles;

        LogObserver(const LogObserver&)            = delete;
        LogObserver& operator=(const LogObserver&) = delete;
        LogObserver(LogObserver&&)                 = delete;
        LogObserver& operator=(LogObserver&&)      = delete;

        bool _addTo(LogDomain&, LogLevel);
        void _removeFrom(LogDomain&);

        bool const _raw;
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
