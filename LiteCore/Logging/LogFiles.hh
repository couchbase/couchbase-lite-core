//
// LogFiles.hh
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
#include "LogObserver.hh"
#include <array>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    /** API for logging to files. */
    class LogFiles : public LogObserver {
      public:
        struct Options {
            std::string path;
            LogLevel    level;
            int64_t     maxSize;
            int         maxCount;
            bool        isPlaintext;
        };

        /// Registers (or unregisters) a file to which log messages will be written in binary format.
        /// @param options The options to use when performing file logging
        /// @param initialMessage  First message that will be written to the log, e.g. version info */
        static void writeEncodedLogsTo(const Options& options, const std::string& initialMessage = "");

        /// Returns the current log file configuration options, as given to `writeEncodedLogsTo`.
        static Options currentOptions();

        static LogLevel logLevel() noexcept;

        static void setLogLevel(LogLevel) noexcept;

        static void flush();

#ifdef LITECORE_CPPTEST
        static std::string createLogPath_forUnitTest(LogLevel level);
        static void        resetRotateSerialNo();
#endif

      protected:
        LogFiles();
        void observe(RawLogEntry const&, const char* format, va_list args) noexcept override __printflike(3, 0);
    };

    /** API for the global logging callback. */
    class LogCallback : public LogObserver {
      public:
        using Callback_t = void (*)(const LogDomain&, LogLevel, const char* format, va_list);

        /** Registers (or unregisters) a callback to be passed log messages.
            @param callback  The callback function, or NULL to unregister.
            @param preformatted  If true, callback will be passed already-formatted log messages to be
                displayed verbatim (and the `va_list` parameter will be NULL.) */
        static void setCallback(Callback_t callback, bool preformatted);

        static Callback_t currentCallback();

        /// The default logging callback writes to stderr, or on Android to `__android_log_write`.
        static void defaultCallback(const LogDomain&, LogLevel, const char* format, va_list) __printflike(3, 0);

        static LogLevel callbackLogLevel() noexcept;

        static void setCallbackLogLevel(LogLevel) noexcept;

      protected:
        LogCallback() = default;
        static void updateLogObserver();
        void        observe(LogEntry const&) noexcept override;
        void        observe(RawLogEntry const&, const char* format, va_list args) noexcept override __printflike(3, 0);
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
