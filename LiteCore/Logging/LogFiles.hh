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
#include "LogObserver.hh"
#include <array>
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

}  // namespace litecore

C4_ASSUME_NONNULL_END
