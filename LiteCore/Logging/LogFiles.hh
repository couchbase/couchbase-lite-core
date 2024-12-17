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
#include <memory>
#include <mutex>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    class LogEncoder;

    /** A LogObserver that writes to a set of log files (text or binary), one per level. */
    class LogFiles : public LogObserver {
      public:
        struct Options {
            std::string directory;                ///< Parent directory for log files; must exist
            std::string initialMessage;           ///< A message to write at the start of each file
            int64_t     maxSize     = 1'000'000;  ///< Size in bytes at which files rotate
            int         maxCount    = 0;          ///< Max number of old files to preserve
            bool        isPlaintext = false;      ///< True to write text files, false for binary
        };

        explicit LogFiles(const Options& options);

        ~LogFiles();

        Options options() const;

        /// Changes the options, if possible, else returns false.
        /// The `directory` and `isPlaintext` fields cannot be changed.
        [[nodiscard]] bool setOptions(Options const&);

        void flush();

        void close();

        // exposed for testing
        static std::string newLogFilePath(std::string_view dir, LogLevel);

      private:
        void _setOptions(Options const&);

        void observe(LogEntry const&) noexcept override;
        void observe(RawLogEntry const&, const char* format, va_list args) noexcept override __printflike(3, 0);

        class LogFile;

        mutable std::mutex                                  _mutex;
        Options                                             _options;
        std::array<std::unique_ptr<LogFile>, kNumLogLevels> _files;  // File per log level
    };

    /** A LogObserver that calls a C++ std::function. */
    class LogFunction : public LogObserver {
      public:
        explicit LogFunction(std::function<void(LogEntry const&)> fn) : _fn(std::move(fn)){};

        void observe(LogEntry const& entry) noexcept override { _fn(entry); }

      private:
        std::function<void(LogEntry const&)> _fn;
    };

    /** A LogObserver that calls a C callback function. */
    class LogCallback : public LogObserver {
      public:
        /// Preferred callback that takes a preformatted LogEntry.
        using Callback_t = void (*)(void* C4NULLABLE context, LogEntry const&);

        /// Lower-level callback that needs to do the printf-style formatting itself.
        using RawCallback_t = void (*)(void* C4NULLABLE context, const LogDomain&, LogLevel, const char* format,
                                       va_list);

        LogCallback(Callback_t cb, void* C4NULLABLE context) : LogCallback(cb, nullptr, context) {}

        LogCallback(RawCallback_t cb, void* C4NULLABLE context) : LogCallback(nullptr, cb, context) {}

        /// A default logging callback that writes to stderr, or on Android to `__android_log_write`.
        static void consoleCallback(void* C4NULLABLE context, LogEntry const&);

      private:
        LogCallback(Callback_t C4NULLABLE, RawCallback_t C4NULLABLE, void* C4NULLABLE context);
        void observe(LogEntry const&) noexcept override;
        void observe(RawLogEntry const&, const char* format, va_list args) noexcept override __printflike(3, 0);

        Callback_t C4NULLABLE    _callback    = nullptr;
        RawCallback_t C4NULLABLE _rawCallback = nullptr;
        void* C4NULLABLE         _context     = nullptr;
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
