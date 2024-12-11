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
#include "Logging_Internal.hh"  // for getObjectPath()
#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "LogObserver.hh"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

#define CBL_LOG_EXTENSION ".cbllog"

namespace litecore {
    using namespace std;
    using namespace std::chrono;
    using namespace litecore::loginternal;

    constexpr const char* C4NONNULL kLevels[] = {"Debug", "Verbose", "Info", "WARNING", "ERROR"};

#pragma mark - FILE LOGGING:

    LogFiles::LogFiles(const Options& options) : LogObserver(!options.isPlaintext) { setOptions(options); }

    LogFiles::~LogFiles() { close(); }

    LogFiles::Options LogFiles::options() const {
        unique_lock lock(_mutex);
        return _options;
    }

    void LogFiles::setOptions(const Options& options) {
        unique_lock lock(_mutex);
        Assert(!options.directory.empty());

        const bool teardown = (options.isPlaintext != _options.isPlaintext || options.directory != _options.directory);
        if ( teardown ) {
            teardownEncoders();
            teardownFileOut();
            for ( auto& no : _rotateSerialNo ) { no++; };
        }

        _options          = options;
        _options.maxSize  = max((int64_t)1024, _options.maxSize);
        _options.maxCount = max(0, _options.maxCount);
        setRaw(!_options.isPlaintext);

        if ( teardown ) {
            purgeOldLogs();
            setupFileOut();
            if ( !_options.isPlaintext ) { setupEncoders(); }

            uint8_t level = 0;
            if ( _options.isPlaintext ) {
                for ( auto& fout : _fileOut ) {
                    LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                    LogDecoder::writeHeader(kLevels[level], "", *fout);
                    *fout << "---- " << fileLogHeader(LogLevel(level)) << " ----" << endl;
                    if ( !_options.initialMessage.empty() ) {
                        LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                        LogDecoder::writeHeader(kLevels[level], "", *fout);
                        *fout << "---- " << _options.initialMessage << " ----" << endl;
                    }
                    ++level;
                }
            } else {
                for ( auto& encoder : _logEncoder ) {
                    encoder->log("", "---- %s ----", fileLogHeader(LogLevel(level)).c_str());
                    if ( !_options.initialMessage.empty() ) {
                        encoder->log("", "---- %s ----", _options.initialMessage.c_str());
                    }
                    encoder->flush();  // Make sure at least the magic bytes are present
                    ++level;
                }
            }
        }
    }

    void LogFiles::flush() {
        unique_lock lock(_mutex);
        for ( auto& encoder : _logEncoder )
            if ( encoder ) encoder->flush();
        for ( auto& fout : _fileOut )
            if ( fout ) fout->flush();
    }

    void LogFiles::close() {
        unique_lock lock(_mutex);
        if ( _logEncoder[0] ) {
            for ( auto& encoder : _logEncoder ) { encoder->log("", "---- END ----"); }
        } else if ( _fileOut[0] ) {
            uint8_t level = 0;
            for ( auto& fout : _fileOut ) {
                LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                LogDecoder::writeHeader(kLevels[level++], "", *fout);
                *fout << "---- END ----" << endl;
            }
        }

        teardownEncoders();
        teardownFileOut();
    }

    void LogFiles::observe(LogEntry const& e) noexcept {
        unique_lock lock(_mutex);
        auto&       file = _fileOut[(int)e.level];
        if ( !file ) return;
        LogDecoder::writeTimestamp(LogDecoder::now(), *file, true);
        LogDecoder::writeHeader(kLevels[(int)e.level], e.domain.name(), *file);
        *file << e.message << endl;

        if ( file->tellp() > _options.maxSize ) rotateLog(e.level);
    }

    void LogFiles::observe(RawLogEntry const& e, const char* format, va_list args) noexcept {
        unique_lock lock(_mutex);
        auto&       encoder = _logEncoder[(int)e.level];
        if ( !encoder ) return;
        string path;
        if ( e.objRef != LogObjectRef::None && encoder->isNewObject(LogEncoder::ObjectRef(e.objRef)) )
            path = getObjectPath(e.objRef);
        encoder->vlog(e.domain.name(), LogEncoder::ObjectRef(e.objRef), path, e.prefix, format, args);

        if ( encoder->tellp() >= _options.maxSize ) rotateLog(e.level);
    }

    //-------- Methods below are private and assume the mutex is locked

    string LogFiles::newLogFilePath(string_view dir, LogLevel level) {
        int64_t millisSinceEpoch = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        stringstream ss;
        ss << dir << FilePath::kSeparator << "cbl_" << kLevelNames[(int)level] << "_" << millisSinceEpoch
           << CBL_LOG_EXTENSION;
        return ss.str();
    }

    string LogFiles::newLogFilePath(LogLevel level) const { return newLogFilePath(_options.directory, level); }

    void LogFiles::setupFileOut() {
        for ( int i = 0; kLevelNames[i]; i++ ) {
            auto path   = newLogFilePath((LogLevel)i);
            _fileOut[i] = make_unique<ofstream>(path, ofstream::out | ofstream::trunc | ofstream::binary);
            if ( !_fileOut[i]->good() ) {
                // calling error::_throw() here can deadlock, by causing a recursive Warn() call
                throw error(error::LiteCore, error::CantOpenFile, "File Logger failed to open file, " + path);
            }
        }
    }

    void LogFiles::setupEncoders() {
        for ( int i = 0; i < kNumLevels; i++ ) {
            _logEncoder[i] = make_unique<LogEncoder>(*_fileOut[i], LogEncoder::LogLevel(i));
        }
    }

    void LogFiles::teardownEncoders() {
        for ( auto& encoder : _logEncoder ) {
            if ( encoder ) encoder->flush();
            encoder = nullptr;
        }
    }

    void LogFiles::teardownFileOut() {
        for ( auto& fout : _fileOut ) {
            if ( fout ) fout->flush();
            fout = nullptr;
        }
    }

    void LogFiles::purgeOldLogs(LogLevel level) {
        FilePath logDir(_options.directory, "");
        if ( !logDir.existsAsDir() ) { return; }

        multimap<time_t, FilePath> logFiles;
        const char*                levelStr = kLevelNames[(int)level];

        logDir.forEachFile([&](const FilePath& f) {
            if ( f.fileName().find(levelStr) != string::npos && f.extension() == CBL_LOG_EXTENSION ) {
                logFiles.insert(make_pair(f.lastModified(), f));
            }
        });

        while ( logFiles.size() > _options.maxCount ) {
            logFiles.begin()->second.del();
            logFiles.erase(logFiles.begin());
        }
    }

    void LogFiles::purgeOldLogs() {
        for ( int i = 0; i < kNumLevels; i++ ) { purgeOldLogs((LogLevel)i); }
    }

    string LogFiles::fileLogHeader(LogLevel level) {
        std::stringstream ss;
        ss << "serialNo=" << _rotateSerialNo[(int)level] << ","
           << "logDirectory=" << _options.directory << ","
           << "fileLogLevel=" << int(level) << ","
           << "fileMaxSize=" << _options.maxSize << ","
           << "fileMaxCount=" << _options.maxCount;
        return ss.str();
    }

    void LogFiles::rotateLog(LogLevel level) {
        auto& encoder = _logEncoder[(int)level];
        auto& file    = _fileOut[(int)level];
        if ( encoder ) {
            encoder->flush();
        } else {
            file->flush();
        }

        _logEncoder[(int)level] = nullptr;
        _fileOut[(int)level]    = nullptr;
        purgeOldLogs(level);
        const auto path      = newLogFilePath(level);
        _fileOut[(int)level] = make_unique<ofstream>(path, ofstream::out | ofstream::trunc | ofstream::binary);
        if ( !_fileOut[(int)level]->good() ) { fprintf(stderr, "rotateLog fails to open %s\n", path.c_str()); }

        _rotateSerialNo[(int)level]++;
        if ( !_options.isPlaintext ) {
            auto newEncoder = make_unique<LogEncoder>(*_fileOut[(int)level], LogEncoder::LogLevel(level));
            newEncoder->log("", "---- %s ----", fileLogHeader(level).c_str());
            if ( !_options.initialMessage.empty() ) {
                newEncoder->log("", "---- %s ----", _options.initialMessage.c_str());
            }
            newEncoder->flush();  // Make sure at least the magic bytes are present
            _logEncoder[(int)level] = std::move(newEncoder);
        } else {
            auto& fout = _fileOut[(int)level];
            LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
            LogDecoder::writeHeader(kLevels[(int)level], "", *fout);
            *fout << "---- " << fileLogHeader(level) << " ----" << endl;
            if ( !_options.initialMessage.empty() ) {
                LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                LogDecoder::writeHeader(kLevels[(int)level], "", *fout);
                *_fileOut[(int)level] << "---- " << _options.initialMessage << " ----" << endl;
            }
        }
    }

#pragma mark - CALLBACKS:

    LogCallback::LogCallback(Callback_t callback, RawCallback_t rawCallback, void* context)
        : LogObserver(rawCallback != nullptr), _callback(callback), _rawCallback(rawCallback), _context(context) {}

    void LogCallback::observe(LogEntry const& e) noexcept { _callback(_context, e); }

    void LogCallback::observe(RawLogEntry const& e, const char* format, va_list args) noexcept {
        _rawCallback(_context, e.domain, e.level, format, args);
    }

    // The default logging callback writes to stderr, or on Android to __android_log_write.
    void LogCallback::consoleCallback(void* ctx, LogEntry const& e) {
#if ANDROID
        string tag("LiteCore");
        string domainName(e.domain.name());
        if ( !domainName.empty() ) tag += " [" + domainName + "]";
        static const int androidLevels[kNumLevels] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_INFO,
                                                      ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        __android_log_vprint(androidLevels[(int)e.level], tag.c_str(), "%s", e.message);
#else
        LogDecoder::writeTimestamp(LogDecoder::now(), cerr);
        LogDecoder::writeHeader(kLevels[(int)e.level], e.domain.name(), cerr);
        cerr << e.message << endl;
#endif
    }

}  // namespace litecore
