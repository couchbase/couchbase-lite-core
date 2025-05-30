//
// Logging_Internal.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Logging.hh"
#include "LogObjectMap.hh"
#include "fleece/RefCounted.hh"
#include <map>
#include <mutex>
#include <vector>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    namespace loginternal {
        constexpr const char* C4NULLABLE kLevelNames[kNumLogLevels + 1] = {"debug",   "verbose", "info",
                                                                           "warning", "error",   nullptr};

        extern LogObjectMap sObjectMap;
    }  // namespace loginternal

    class LogObserver;
    struct LogEntry;
    struct RawLogEntry;

    /** A set of LogObserver instances with associated LogLevels.
        Used internally by LogDomain. */
    class LogObservers {
      public:
        LogObservers();

        /// Registers a LogObserver to receive logs of this level and higher.
        /// @returns  True on success, false if it's already registered.
        [[nodiscard]] bool addObserver(LogObserver*, LogLevel level);

        /// Unregisters a LogObserver.
        /// @returns  True if removed, false if it was not registered.
        bool removeObserver(LogObserver*) noexcept;

        /// The lowest (most verbose) level of any attached observer.
        LogLevel lowestLevel() const noexcept;

        /// Posts a log message to all relevant observers. (Called internally by logging functions.)
        void notify(RawLogEntry const&, const char* format, va_list args) __printflike(3, 0);

        /// Posts a log message only to formatted (non-raw) observers. (This is kind of a special case.)
        void notifyCallbacksOnly(LogEntry const&);

      private:
        std::mutex mutable _mutex;
        // Sorted by increasing LogLevel: Debug, Verbose, Info, Warning, Error
        std::vector<std::pair<fleece::Retained<LogObserver>, LogLevel>> _observers;
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
