//
// LogFunction.hh
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
#include "LogObserver.hh"
#include <functional>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    /** A LogObserver that calls a C++ std::function. */
    class LogFunction : public LogObserver {
      public:
        explicit LogFunction(std::function<void(LogEntry const&)> fn) : _fn(std::move(fn)){};

        void observe(LogEntry const& entry) noexcept override { _fn(entry); }

        /// Writes a formatted log entry to stderr, or on Android to `__android_log_write`.
        static void logToConsole(LogEntry const&);

      private:
        std::function<void(LogEntry const&)> _fn;
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
