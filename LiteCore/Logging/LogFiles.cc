//
// LogFiles.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LogFiles.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "LogFunction.hh"
#include "Logging_Internal.hh"  // for getObjectPath()
#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "LogObserver.hh"
#include "StringUtil.hh"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

#if __ANDROID__
#    include <android/log.h>
#endif

namespace litecore {
    using namespace std;
    using namespace std::chrono;
    using namespace litecore::loginternal;

    static constexpr string_view kLogFileExtension = ".cbllog";

    /// Level names to write into textual logs, both in headers and in the lines logged.
    static constexpr const char* kLevelNamesInLog[] = {"Debug", "Verbose", "Info", "WARNING", "ERROR"};

#pragma mark - LOGFILE:

    /** Represents a single log file for one log level, owned by a LogFiles object. */
    class LogFiles::LogFile {
      public:
        LogFile(LogLevel level, Options const& options) : _level(level), _options(options) {}

        void open() {
            auto path = LogFiles::newLogFilePath(_options.directory, _level);
            _fileOut.open(path, ofstream::out | ofstream::trunc | ofstream::binary);
            if ( !_fileOut.good() ) {
                // calling error::_throw() here could deadlock, by causing a recursive Warn() call
                throw error(error::LiteCore, error::CantOpenFile, "File Logger failed to open file, " + path);
            }

            string header = CONCAT("serialNo=" << _rotateSerialNo << ","
                                               << "logDirectory=" << _options.directory << ","
                                               << "fileLogLevel=" << int(_level) << ","
                                               << "fileMaxSize=" << _options.maxSize << ","
                                               << "fileMaxCount=" << _options.maxCount);

            if ( _options.isPlaintext ) {
                LogDecoder::writeTimestamp(LogDecoder::now(), _fileOut, true);
                LogDecoder::writeHeader(levelName(), "", _fileOut);
                _fileOut << "---- " << header << " ----" << endl;
                if ( !_options.initialMessage.empty() ) {
                    LogDecoder::writeTimestamp(LogDecoder::now(), _fileOut, true);
                    LogDecoder::writeHeader(levelName(), "", _fileOut);
                    _fileOut << "---- " << _options.initialMessage << " ----" << endl;
                }
            } else {
                _logEncoder = make_unique<LogEncoder>(_fileOut, LogEncoder::LogLevel(_level));
                _logEncoder->log("", "---- %s ----", header.c_str());
                if ( !_options.initialMessage.empty() ) {
                    _logEncoder->log("", "---- %s ----", _options.initialMessage.c_str());
                }
                _logEncoder->flush();  // Make sure at least the magic bytes are present
            }
        }

        void write(LogEntry const& e) {
            if ( !_fileOut.is_open() ) return;
            LogDecoder::writeTimestamp(LogDecoder::now(), _fileOut, true);
            LogDecoder::writeHeader(levelName(), e.domain.name(), _fileOut);
            _fileOut << e.message << endl;
            if ( _fileOut.tellp() > _options.maxSize ) rotateLog();
        }

        void write(RawLogEntry const& e, const char* format, va_list args) __printflike(3, 0) {
            if ( !_logEncoder ) return;
            string path;
            if ( e.objRef != LogObjectRef::None && _logEncoder->isNewObject(LogEncoder::ObjectRef(e.objRef)) )
                path = sObjectMap.getObjectPath(e.objRef);
            _logEncoder->vlog(e.domain.name(), LogEncoder::ObjectRef(e.objRef), path, e.prefix, format, args);
            if ( _logEncoder->tellp() > _options.maxSize ) rotateLog();
        }

        void flush() {
            if ( _logEncoder ) _logEncoder->flush();
            else if ( _fileOut.is_open() )
                _fileOut.flush();
        }

        void close(bool writeTrailer = true) {
            if ( _options.isPlaintext ) {
                if ( writeTrailer && _fileOut.is_open() ) {
                    LogDecoder::writeTimestamp(LogDecoder::now(), _fileOut, true);
                    LogDecoder::writeHeader(levelName(), "", _fileOut);
                    _fileOut << "---- END ----" << endl;
                }
            } else if ( _logEncoder ) {
                if ( writeTrailer ) _logEncoder->log("", "---- END ----");
                _logEncoder->flush();
            }
            _logEncoder = nullptr;
            _fileOut.close();
        }

      private:
        const char* levelName() const { return kLevelNamesInLog[int(_level)]; }

        void rotateLog() {
            close(false);
            purgeOldLogs();
            _rotateSerialNo++;
            open();
        }

        void purgeOldLogs() {
            FilePath logDir(_options.directory, "");
            if ( !logDir.existsAsDir() ) { return; }

            multimap<time_t, FilePath> logFiles;
            const char*                levelStr = kLevelNames[(int)_level];
            logDir.forEachFile([&](const FilePath& f) {
                if ( f.fileName().find(levelStr) != string::npos && f.extension() == kLogFileExtension ) {
                    logFiles.emplace(f.lastModified(), f);
                }
            });

            while ( logFiles.size() > _options.maxCount ) {
                (void)logFiles.begin()->second.del();
                logFiles.erase(logFiles.begin());
            }
        }

        LogLevel const         _level;               ///< My log level
        Options const&         _options;             ///< Reference to owning LogFiles's options
        ofstream               _fileOut;             ///< Log file stream (text or binary)
        unique_ptr<LogEncoder> _logEncoder;          ///< Binary log encoder, writes to _fileOut, or NULL
        unsigned               _rotateSerialNo = 1;  ///< Counter appearing at top of log file
    };

#pragma mark - LOGFILES:

    LogFiles::LogFiles(const Options& options) : LogObserver(!options.isPlaintext) {
        _setOptions(options);
        // Initialize LogFile objects before opening them:
        for ( int i = 0; i < kNumLogLevels; i++ ) _files[i] = make_unique<LogFile>(LogLevel(i), _options);
        for ( int i = 0; i < kNumLogLevels; i++ ) _files[i]->open();
    }

    LogFiles::~LogFiles() { close(); }

    LogFiles::Options LogFiles::options() const {
        unique_lock lock(_mutex);
        return _options;
    }

    bool LogFiles::setOptions(const Options& options) {
        unique_lock lock(_mutex);
        if ( options.isPlaintext != _options.isPlaintext || options.directory != _options.directory ) return false;
        _setOptions(options);
        return true;
    }

    void LogFiles::_setOptions(Options const& options) {
        Assert(!options.directory.empty());
        _options          = options;
        _options.maxSize  = max(int64_t(1024), _options.maxSize);
        _options.maxCount = max(0, _options.maxCount);
    }

    void LogFiles::flush() {
        unique_lock lock(_mutex);
        for ( auto& file : _files ) file->flush();
    }

    void LogFiles::close() {
        unique_lock lock(_mutex);
        for ( auto& file : _files ) file->close();
    }

    void LogFiles::observe(LogEntry const& e) noexcept {
        unique_lock lock(_mutex);
        _files[int(e.level)]->write(e);
    }

    void LogFiles::observe(RawLogEntry const& e, const char* format, va_list args) noexcept {
        unique_lock lock(_mutex);
        _files[int(e.level)]->write(e, format, args);
    }

    string LogFiles::newLogFilePath(string_view dir, LogLevel level) {
        int64_t millisSinceEpoch = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        return CONCAT(dir << FilePath::kSeparator << "cbl_" << kLevelNames[(int)level] << "_" << millisSinceEpoch
                          << kLogFileExtension);
    }

#pragma mark - CALLBACKS:

    // The default logging callback writes to stderr, or on Android to __android_log_write.
    void LogFunction::logToConsole(LogEntry const& e) {
#if ANDROID
        string tag("LiteCore");
        string domainName(e.domain.name());
        if ( !domainName.empty() ) tag += " [" + domainName + "]";
        static const int androidLevels[kNumLogLevels] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_INFO,
                                                         ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        __android_log_write(androidLevels[int(e.level)], tag.c_str(), e.messageStr());
#else
        static mutex          sConsoleMutex;
        unique_lock           lock(sConsoleMutex);
        LogDecoder::Timestamp ts{.secs = time_t(e.timestamp / 1000), .microsecs = 1000 * unsigned(e.timestamp % 1000)};
        LogDecoder::writeTimestamp(ts, cerr);
        LogDecoder::writeHeader(kLevelNamesInLog[(int)e.level], e.domain.name(), cerr);
        cerr << e.message << endl;
#endif
    }

}  // namespace litecore
