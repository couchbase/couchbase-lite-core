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
#include <cstdarg>
#include <span>
#include <string>
#include <string_view>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    /** Struct representing a log message and its metadata. */
    struct LogEntry {
        uint64_t         timestamp;  ///< Time the event occurred
        LogDomain&       domain;     ///< Domain being logged to
        LogLevel         level;      ///< Severity level
        std::string_view message;    ///< The text. Guaranteed to be followed by a 00 byte.
    };

    /** Struct representing the metadata of an unformatted log message.
        The message itself is not included; it's passed as separate `fmt` and `args` parameters. */
    struct RawLogEntry {
        uint64_t           timestamp;  ///< Time the event occurred
        LogDomain&         domain;     ///< Domain being logged to
        LogLevel           level;      ///< Severity level
        LogObjectRef       objRef;     ///< Registered object that logged the mesage, else None (0)
        const std::string& prefix;     ///< Optional prefix string to add to the message
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

        /// Sets whether the observer wants RawLogEntry or regular parsed LogEntry (the default.)
        void setRaw(bool raw) { _raw = raw; }

        bool raw() const { return _raw; }

        /// Informs a LogObserver of a new log message. Only called if `raw` is false.
        virtual void observe(LogEntry const&) noexcept;

        /// Informs a LogObserver of a new log message. Only called if `raw` is true.
        virtual void observe(RawLogEntry const&, const char* format, va_list args) noexcept;

      private:
        friend class LogCallback;
        friend class LogFiles;

        static void _add(LogObserver*, LogLevel defaultLevel, std::span<const std::pair<LogDomain&, LogLevel>> = {});
        static void _remove(LogObserver*);
        void        _addTo(LogDomain&, LogLevel);
        void        _removeFrom(LogDomain&);

        bool _raw = false;
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
