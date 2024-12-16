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
#include "Logging_Internal.hh"
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

#pragma mark - FILE LOGGING:

    static fleece::Retained<LogFiles> sFileObserver;
    static LogFiles::Options          sCurrentOptions;
    static LogLevel                   sFileMinLevel      = LogLevel::None;
    static ofstream*                  sFileOut[5]        = {};  // File per log level
    static LogEncoder*                sLogEncoder[5]     = {};
    static unsigned                   sRotateSerialNo[5] = {};
    static string                     sLogDirectory;
    static int                        sMaxCount = 0;     // For rotation
    static int64_t                    sMaxSize  = 1024;  // For rotation
    static string                     sInitialMessage;   // For rotation, goes at top of each log

    static string createLogPath(LogLevel level) {
        int64_t millisSinceEpoch = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        stringstream ss;
        ss << sLogDirectory << FilePath::kSeparator << "cbl_" << kLevelNames[(int)level] << "_" << millisSinceEpoch
           << CBL_LOG_EXTENSION;
        return ss.str();
    }

#ifdef LITECORE_CPPTEST
    string LogFiles::createLogPath_forUnitTest(LogLevel level) { return createLogPath(level); }

    void LogFiles::resetRotateSerialNo() {
        for ( auto& no : sRotateSerialNo ) { no = 0; }
    }
#endif

    static void setupFileOut() {
        for ( int i = 0; kLevelNames[i]; i++ ) {
            auto path   = createLogPath((LogLevel)i);
            sFileOut[i] = new ofstream(path, ofstream::out | ofstream::trunc | ofstream::binary);
            if ( !sFileOut[i]->good() ) {
                // calling error::_throw() here would deadlock
                throw error(error::LiteCore, error::CantOpenFile, "File Logger fails to open file, " + path);
            }
        }
    }

    static void setupEncoders() {
        for ( int i = 0; i < 5; i++ ) { sLogEncoder[i] = new LogEncoder(*sFileOut[i], LogEncoder::LogLevel(i)); }
    }

    static void teardownEncoders() {
        for ( auto& encoder : sLogEncoder ) {
            if ( encoder ) encoder->flush();
            delete encoder;
            encoder = nullptr;
        }
    }

    static void teardownFileOut() {
        for ( auto& fout : sFileOut ) {
            if ( fout ) fout->flush();
            delete fout;
            fout = nullptr;
        }
    }

    static bool needsTeardown(const LogFiles::Options& options) {
        if ( sLogEncoder[0] == nullptr && !options.isPlaintext ) { return true; }

        if ( sLogEncoder[0] != nullptr && options.isPlaintext ) { return true; }

        if ( sLogDirectory != options.path ) { return true; }

        return false;
    }

    static void purgeOldLogs(LogLevel level) {
        FilePath logDir(sLogDirectory, "");
        if ( !logDir.existsAsDir() ) { return; }

        multimap<time_t, FilePath> logFiles;
        const char*                levelStr = kLevelNames[(int)level];

        logDir.forEachFile([&](const FilePath& f) {
            if ( f.fileName().find(levelStr) != string::npos && f.extension() == CBL_LOG_EXTENSION ) {
                logFiles.insert(make_pair(f.lastModified(), f));
            }
        });

        while ( logFiles.size() > sMaxCount ) {
            logFiles.begin()->second.del();
            logFiles.erase(logFiles.begin());
        }
    }

    static void purgeOldLogs() {
        for ( int i = 0; i < 5; i++ ) { purgeOldLogs((LogLevel)i); }
    }

    static string fileLogHeader(LogLevel level) {
        std::stringstream ss;
        ss << "serialNo=" << sRotateSerialNo[(int)level] << ","
           << "logDirectory=" << sLogDirectory << ","
           << "fileLogLevel=" << (int)LogFiles::logLevel() << ","
           << "fileMaxSize=" << sMaxSize << ","
           << "fileMaxCount=" << sMaxCount;
        return ss.str();
    }

    static void rotateLog(LogLevel level) {
        auto encoder = sLogEncoder[(int)level];
        auto file    = sFileOut[(int)level];
        if ( encoder ) {
            encoder->flush();
        } else {
            file->flush();
        }

        delete sLogEncoder[(int)level];
        delete sFileOut[(int)level];
        sLogEncoder[(int)level] = nullptr;
        sFileOut[(int)level]    = nullptr;
        purgeOldLogs(level);
        const auto path      = createLogPath(level);
        sFileOut[(int)level] = new ofstream(path, ofstream::out | ofstream::trunc | ofstream::binary);
        if ( !sFileOut[(int)level]->good() ) { fprintf(stderr, "rotateLog fails to open %s\n", path.c_str()); }

        sRotateSerialNo[(int)level]++;
        if ( encoder ) {
            auto newEncoder         = new LogEncoder(*sFileOut[(int)level], LogEncoder::LogLevel(level));
            sLogEncoder[(int)level] = newEncoder;
            newEncoder->log("", "---- %s ----", fileLogHeader(level).c_str());
            if ( !sInitialMessage.empty() ) { newEncoder->log("", "---- %s ----", sInitialMessage.c_str()); }
            newEncoder->flush();  // Make sure at least the magic bytes are present
        } else {
            auto fout = sFileOut[(int)level];
            LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
            LogDecoder::writeHeader(kLevels[(int)level], "", *fout);
            *fout << "---- " << fileLogHeader(level) << " ----" << endl;
            if ( !sInitialMessage.empty() ) {
                LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                LogDecoder::writeHeader(kLevels[(int)level], "", *fout);
                *sFileOut[(int)level] << "---- " << sInitialMessage << " ----" << endl;
            }
        }
    }

    void LogFiles::flush() {
        unique_lock<mutex> lock(sLogMutex);

        for ( auto& encoder : sLogEncoder )
            if ( encoder ) encoder->flush();
        for ( auto& fout : sFileOut )
            if ( fout ) fout->flush();
    }

    void LogFiles::writeEncodedLogsTo(const Options& options, const string& initialMessage) {
        unique_lock<mutex> lock(sLogMutex);

        if ( sFileObserver ) LogObserver::_remove(sFileObserver);

        sMaxSize            = max((int64_t)1024, options.maxSize);
        sMaxCount           = max(0, options.maxCount);
        const bool teardown = needsTeardown(options);
        if ( teardown ) {
            teardownEncoders();
            teardownFileOut();
            for ( auto& no : sRotateSerialNo ) { no++; };
        }

        sCurrentOptions = options;
        sLogDirectory   = options.path;
        sInitialMessage = initialMessage;
        if ( sLogDirectory.empty() ) {
            sFileMinLevel = LogLevel::None;
        } else {
            sFileMinLevel = options.level;
            if ( teardown ) {
                purgeOldLogs();
                setupFileOut();
                if ( !options.isPlaintext ) { setupEncoders(); }

                uint8_t level = 0;
                if ( sLogEncoder[0] ) {
                    for ( auto& encoder : sLogEncoder ) {
                        encoder->log("", "---- %s ----", fileLogHeader(LogLevel(level++)).c_str());
                        if ( !sInitialMessage.empty() ) { encoder->log("", "---- %s ----", sInitialMessage.c_str()); }
                        encoder->flush();  // Make sure at least the magic bytes are present
                    }
                } else {
                    for ( auto& fout : sFileOut ) {
                        LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                        LogDecoder::writeHeader(kLevels[level], "", *fout);
                        *fout << "---- " << fileLogHeader(LogLevel(level)) << " ----" << endl;
                        if ( !sInitialMessage.empty() ) {
                            LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                            LogDecoder::writeHeader(kLevels[level], "", *fout);
                            *fout << "---- " << sInitialMessage << " ----" << endl;
                        }
                        ++level;
                    }
                }

                // Make sure to flush the log when the process exits:
                static once_flag f;
                call_once(f, [] {
                    atexit([] {
                        if ( sLogMutex.try_lock() ) {  // avoid deadlock on crash inside logging code
                            if ( sLogEncoder[0] ) {
                                for ( auto& encoder : sLogEncoder ) { encoder->log("", "---- END ----"); }
                            } else if ( sFileOut[0] ) {
                                uint8_t level = 0;
                                for ( auto& fout : sFileOut ) {
                                    LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                                    LogDecoder::writeHeader(kLevels[level++], "", *fout);
                                    *fout << "---- END ----" << endl;
                                }
                            }

                            teardownEncoders();
                            teardownFileOut();
                            sLogMutex.unlock();
                        }
                    });
                });
            }
        }

        if ( !sFileObserver ) sFileObserver = new LogFiles();
        LogObserver::_add(sFileObserver, sFileMinLevel);
    }

    LogFiles::Options LogFiles::currentOptions() {
        unique_lock<mutex> lock(sLogMutex);
        return sCurrentOptions;
    }

    LogLevel LogFiles::logLevel() noexcept { return sFileMinLevel; }

    void LogFiles::setLogLevel(LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);
        if ( level != sFileMinLevel ) {
            sFileMinLevel = level;
            if ( sFileObserver ) {
                LogObserver::_remove(sFileObserver);
                LogObserver::add(sFileObserver, sFileMinLevel);
            }
        }
    }

    LogFiles::LogFiles() { setRaw(true); }

    void LogFiles::observe(RawLogEntry const& e, const char* format, va_list args) noexcept {
        uint64_t    pos;
        const char* domain = e.domain.name();

        // Safe to store these in variables, since they only change in the rotateLog method
        // and the rotateLog method is only called here (and this method holds a mutex)
        const auto encoder = sLogEncoder[(int)e.level];
        const auto file    = sFileOut[(int)e.level];
        if ( encoder ) {
            string path;
            if ( e.objRef != LogObjectRef::None && encoder->isNewObject(LogEncoder::ObjectRef(e.objRef)) )
                path = getObjectPath(e.objRef);
            encoder->vlog(domain, LogEncoder::ObjectRef(e.objRef), path, e.prefix, format, args);
            pos = encoder->tellp();
        } else if ( file ) {
            static char formatBuffer[2048];
            size_t      n = 0;
            LogDecoder::writeTimestamp(LogDecoder::now(), *file, true);
            LogDecoder::writeHeader(kLevels[(int)e.level], domain, *file);
            if ( e.objRef != LogObjectRef::None ) n = addObjectPath(formatBuffer, sizeof(formatBuffer), e.objRef);
            if ( !e.prefix.empty() && n + e.prefix.size() + 1 < sizeof(formatBuffer) ) {
                memcpy(&formatBuffer[n], e.prefix.data(), e.prefix.size());
                n += e.prefix.size();
                formatBuffer[n++] = ' ';
            }
            vsnprintf(&formatBuffer[n], sizeof(formatBuffer) - n, format, args);
            *file << formatBuffer << endl;
            pos = file->tellp();
        } else {
            // No rotation if neither encoder nor file is present
            return;
        }

        if ( pos >= sMaxSize ) { rotateLog(e.level); }
    }

#pragma mark - CALLBACKS:

    static fleece::Retained<LogCallback> sCallbackObserver;
    static LogLevel                      sCallbackMinLevel = LogLevel::None;
    static LogCallback::Callback_t       sCallback;
    static bool                          sCallbackPreformatted = false;

    void LogCallback::updateLogObserver() {
        if ( sCallback && sCallbackMinLevel != LogLevel::None ) {
            if ( sCallbackObserver ) LogObserver::_remove(sCallbackObserver);
            else
                sCallbackObserver = new LogCallback();
            sCallbackObserver->setRaw(!sCallbackPreformatted);
            LogObserver::_add(sCallbackObserver, sCallbackMinLevel);
        } else {
            if ( sCallbackObserver ) {
                LogObserver::_remove(sCallbackObserver);
                sCallbackObserver = nullptr;
            }
        }
    }

    void LogCallback::setCallback(Callback_t callback, bool preformatted) {
        unique_lock<mutex> lock(sLogMutex);
        if ( !callback ) sCallbackMinLevel = LogLevel::None;
        sCallback             = callback;
        sCallbackPreformatted = preformatted;
        updateLogObserver();
    }

    LogCallback::Callback_t LogCallback::currentCallback() { return sCallback; }

    void LogCallback::setCallbackLogLevel(LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);
        if ( level != sCallbackMinLevel ) {
            sCallbackMinLevel = level;
            updateLogObserver();
        }
    }

    // Only call while holding sLogMutex!
    static LogLevel _callbackLogLevel() noexcept {
        auto level = sCallbackMinLevel;
        if ( level == LogLevel::Uninitialized ) {
            level             = LogLevel::Info;
            sCallbackMinLevel = level;
        }
        return level;
    }

    LogLevel LogCallback::callbackLogLevel() noexcept {
        unique_lock<mutex> lock(sLogMutex);
        return _callbackLogLevel();
    }

    void LogCallback::observe(LogEntry const& e) noexcept {
        va_list noArgs{};
        sCallback(e.domain, e.level, e.message.data(), noArgs);
    }

    void LogCallback::observe(RawLogEntry const& e, const char* format, va_list args) noexcept {
        size_t      n      = 0;
        const char* useFmt = format;
        if ( e.objRef != LogObjectRef::None ) n = addObjectPath(sFormatBuffer, sizeof(sFormatBuffer), e.objRef);
        if ( n > 0 ) {
            snprintf(&sFormatBuffer[n], sizeof(sFormatBuffer) - n, "%s ", format);
            useFmt = sFormatBuffer;
        }
        sCallback(e.domain, e.level, useFmt, args);
    }

    // The default logging callback writes to stderr, or on Android to __android_log_write.
    void LogCallback::defaultCallback(const LogDomain& domain, LogLevel level, const char* fmt, va_list args) {
#if ANDROID
        string tag("LiteCore");
        string domainName(domain.name());
        if ( !domainName.empty() ) tag += " [" + domainName + "]";
        static const int androidLevels[5] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                             ANDROID_LOG_ERROR};
        __android_log_vprint(androidLevels[(int)level], tag.c_str(), fmt, args);
#else
        char* cstr = nullptr;
        if ( vasprintf(&cstr, fmt, args) < 0 ) throw bad_alloc();
        LogDecoder::writeTimestamp(LogDecoder::now(), cerr);
        LogDecoder::writeHeader(kLevels[(int)level], domain.name(), cerr);
        cerr << cstr << endl;
        free(cstr);
#endif
    }

}  // namespace litecore
