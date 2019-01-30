//
// Logging.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Logging.hh"
#include "StringUtil.hh"
#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "PlatformIO.hh"
#include "FilePath.hh"
#include <string>
#include <fstream>
#include <mutex>
#include <ctime>

#if __APPLE__
#include <sys/time.h>
#endif
#if __ANDROID__
#include <android/log.h>
#endif

#if defined(__clang__) && !defined(__ANDROID__)
#include <cxxabi.h>
#endif

#ifdef _MSC_VER
#include <winapifamily.h>
#endif

#define CBL_LOG_EXTENSION ".cbllog"

using namespace std;
using namespace std::chrono;

namespace litecore {

    LogDomain* LogDomain::sFirstDomain = nullptr;
    static LogDomain _ActorLog("Actor");
    LogDomain &ActorLog = _ActorLog;

    LogLevel LogDomain::sCallbackMinLevel = LogLevel::Uninitialized;
    static LogDomain::Callback_t sCallback = LogDomain::defaultCallback;
    static bool sCallbackPreformatted = false;
    LogLevel LogDomain::sFileMinLevel = LogLevel::None;
    unsigned LogDomain::slastObjRef {0};
    map<unsigned, string> LogDomain::sObjNames;
    static ofstream* sFileOut[5] = {}; // File per log level
    static LogEncoder* sLogEncoder[5] = {};
    static string sLogDirectory;
    static int sMaxCount = 0;       // For rotation
    static int64_t sMaxSize = 1024; // For rotation
    static string sInitialMessage;  // For rotation, goes at top of each log
    static mutex sLogMutex;

    static const char* const kLevelNames[] = {"debug", "verbose", "info",
                "warning", "error", nullptr};
    static const char *kLevels[] = {"***", "", "", "WARNING", "ERROR"};

    static string createLogPath(LogLevel level)
    {
        int64_t millisSinceEpoch =
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        stringstream ss;
        ss << sLogDirectory << FilePath::kSeparator << "cbl_" << kLevelNames[(int)level] << "_" << millisSinceEpoch << CBL_LOG_EXTENSION;
        return ss.str();
    }

    static void setupFileOut()
    {
        for (int i = 0; kLevelNames[i]; i++) {
            auto path = createLogPath((LogLevel)i);
            sFileOut[i] = new ofstream(path, ofstream::out|ofstream::trunc|ofstream::binary);
        }
    }

    static void setupEncoders()
    {
        for(int i = 0; i < 5; i++) {
            sLogEncoder[i]  = new LogEncoder(*sFileOut[i], (LogLevel)i);
        }
    }

    static void teardownEncoders()
    {
        for(auto& encoder : sLogEncoder) {
            if(encoder) encoder->flush();
            delete encoder;
            encoder = nullptr;
        }
    }

    static void teardownFileOut()
    {
        for (auto& fout : sFileOut) {
            if(fout) fout->flush();
            delete fout;
            fout = nullptr;
        }
    }

    static bool needsTeardown(const LogFileOptions& options)
    {
        if(sLogEncoder[0] == nullptr && !options.isPlaintext) {
            return true;
        }

        if(sLogEncoder[0] != nullptr && options.isPlaintext) {
            return true;
        }

        if(sLogDirectory != options.path) {
            return true;
        }

        return false;
    }

    static void purgeOldLogs(LogLevel level)
    {
        FilePath logDir(sLogDirectory, "");
        if(!logDir.existsAsDir()) {
            logDir.mkdir();
            return;
        }

        multimap<time_t, FilePath> logFiles;
        const char* levelStr = kLevelNames[(int)level];

        logDir.forEachFile([&](const FilePath& f) {
            if (f.fileName().find(levelStr) != string::npos && f.extension() == CBL_LOG_EXTENSION) {
                logFiles.insert(make_pair(f.lastModified(), f));
            }
        });

        while(logFiles.size() > sMaxCount) {
            logFiles.begin()->second.del();
            logFiles.erase(logFiles.begin());
        }
    }

    static void purgeOldLogs()
    {
        for(int i = 0; i < 5; i++) {
            purgeOldLogs((LogLevel)i);
        }
    }

    void Logging::rotateLog(LogLevel level)
    {
        auto encoder = sLogEncoder[(int)level];
        auto file = sFileOut[(int)level];
        if(encoder) {
            encoder->flush();
        } else {
            file->flush();
        }

        delete sLogEncoder[(int)level];
        delete sFileOut[(int)level];
        purgeOldLogs(level);
        const auto path = createLogPath(level);
        sFileOut[(int)level] = new ofstream(path, ofstream::out|ofstream::trunc|ofstream::binary);
        if(encoder) {
            auto newEncoder = new LogEncoder(*sFileOut[(int)level], level);
            sLogEncoder[(int)level] = newEncoder;
            newEncoder->log("", map<unsigned, string>(), LogEncoder::None, "---- %s ----", sInitialMessage.c_str());
            newEncoder->flush(); // Make sure at least the magic bytes are present
        } else {
            *sFileOut[(int)level] << "---- " << sInitialMessage << " ----" << endl;
        }
    }

#pragma mark - GLOBAL SETTINGS:


    void LogDomain::setCallback(Callback_t callback, bool preformatted) {
        unique_lock<mutex> lock(sLogMutex);
        if (!callback)
            sCallbackMinLevel = LogLevel::None;
        sCallback = callback;
        sCallbackPreformatted = preformatted;
        _invalidateEffectiveLevels();
    }


    void LogDomain::writeEncodedLogsTo(const LogFileOptions& options,
                                       const string &initialMessage)
    {
        unique_lock<mutex> lock(sLogMutex);
        sMaxSize = max((int64_t)1024, options.maxSize);
        sMaxCount = max(0, options.maxCount);
        const bool teardown = needsTeardown(options);
        if(teardown) {
            teardownEncoders();
            teardownFileOut();
        }

        sLogDirectory = options.path;
        sInitialMessage = initialMessage;
        if (sLogDirectory.empty()) {
            sFileMinLevel = LogLevel::None;
        } else {
            sFileMinLevel = options.level;
            if(!teardown) {
                return;
            }

            purgeOldLogs();
            setupFileOut();
            if(!options.isPlaintext) {
                setupEncoders();
            }

            if (!sInitialMessage.empty()) {
                if(sLogEncoder[0]) {
                    for(auto& encoder : sLogEncoder) {
                        encoder->log("", map<unsigned, string>(), LogEncoder::None, "---- %s ----", sInitialMessage.c_str());
                        encoder->flush(); // Make sure at least the magic bytes are present
                    }
                } else {
                    for(auto& fout : sFileOut) {
                        *fout << "---- " << sInitialMessage << " ----" << endl;
                    }
                }
            }

            // Make sure to flush the log when the process exits:
            static once_flag f;
            call_once(f, []{
                atexit([]{
                    if (sLogMutex.try_lock()) {     // avoid deadlock on crash inside logging code
                        if (sLogEncoder[0]) {
                            for(auto& encoder : sLogEncoder) {
                                encoder->log("", map<unsigned, string>(), LogEncoder::None,
                                             "---- END ----");
                            }
                        }
                            
                        teardownEncoders();
                        teardownFileOut();
                        sLogMutex.unlock();
                    }
                });
            });
        }
        _invalidateEffectiveLevels();
    }


    void LogDomain::setCallbackLogLevel(LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);

        // Setting "LiteCoreLog" env var forces a minimum level of logging:
        auto envLevel = kC4Cpp_DefaultLog.levelFromEnvironment();
        if (envLevel != LogLevel::Uninitialized)
            level = min(level, envLevel);

        if (level != sCallbackMinLevel) {
            sCallbackMinLevel = level;
            _invalidateEffectiveLevels();
        }
    }

    void LogDomain::setFileLogLevel(LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);
        if (level != sFileMinLevel) {
            sFileMinLevel = level;
            _invalidateEffectiveLevels();
        }
    }


    // Only call while holding sLogMutex!
    void LogDomain::_invalidateEffectiveLevels() noexcept {
        for (auto d = sFirstDomain; d; d = d->_next)
            d->_effectiveLevel = LogLevel::Uninitialized;
    }


    LogLevel LogDomain::callbackLogLevel() noexcept {
        unique_lock<mutex> lock(sLogMutex);
        return _callbackLogLevel();
    }

    // Only call while holding sLogMutex!
    LogLevel LogDomain::_callbackLogLevel() noexcept {
        auto level = sCallbackMinLevel;
        if (level == LogLevel::Uninitialized) {
            // Allow 'LiteCoreLog' env var to set initial callback level:
            level = kC4Cpp_DefaultLog.levelFromEnvironment();
            if (level == LogLevel::Uninitialized)
                level = LogLevel::Info;
            sCallbackMinLevel = level;
        }
        return level;
    }


#pragma mark - INITIALIZATION:


    // Returns the LogLevel override set by an environment variable, or Uninitialized if none
    LogLevel LogDomain::levelFromEnvironment() const noexcept {
#if !defined(_MSC_VER) || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        char *val = getenv((string("LiteCoreLog") + _name).c_str());
        if (val) {
            static const char* const kLevelNames[] = {"debug", "verbose", "info",
                "warning", "error", "none", nullptr};
            for (int i = 0; kLevelNames[i]; i++) {
                if (0 == strcasecmp(val, kLevelNames[i]))
                    return LogLevel(i);
            }
            return LogLevel::Info;
        }
#endif
        return LogLevel::Uninitialized;
    }


    LogLevel LogDomain::computeLevel() noexcept {
        if (_effectiveLevel == LogLevel::Uninitialized)
            setLevel(_level);
        return _level;
    }


    LogLevel LogDomain::level() const noexcept {
        return const_cast<LogDomain*>(this)->computeLevel();
    }


    void LogDomain::setLevel(litecore::LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);

        // Setting "LiteCoreLog___" env var forces a minimum level:
        auto envLevel = levelFromEnvironment();
        if (envLevel != LogLevel::Uninitialized)
            level = min(level, envLevel);

        _level = level;
        // The effective level is the level at which I will actually trigger because there is
        // a place for my output to go:
        _effectiveLevel = max((LogLevel)_level, min(_callbackLogLevel(), sFileMinLevel));
    }


    LogDomain* LogDomain::named(const char *name) {
        unique_lock<mutex> lock(sLogMutex);
        if (!name)
            name = "";
        for (auto d = sFirstDomain; d; d = d->_next)
            if (strcmp(d->name(), name) == 0)
                return d;
        return nullptr;
    }


#pragma mark - LOGGING:


    static char sFormatBuffer[2048];

    void LogDomain::vlog(LogLevel level, unsigned objRef, bool doCallback, const char *fmt, va_list args) {
        if (_effectiveLevel == LogLevel::Uninitialized)
            computeLevel();
        if (!willLog(level))
            return;

        unique_lock<mutex> lock(sLogMutex);

        // Invoke the client callback:
        if (doCallback && sCallback && level >= _callbackLogLevel()) {
            auto obj = getObject(objRef);

            va_list args2;
            va_copy(args2, args);
            if (sCallbackPreformatted) {
                // Preformatted: Do the formatting myself and pass the resulting string:
                size_t n = 0;
                if (objRef)
                    n = snprintf(sFormatBuffer, sizeof(sFormatBuffer), "{%s#%u} ", obj.c_str(), objRef);
                vsnprintf(&sFormatBuffer[n], sizeof(sFormatBuffer) - n, fmt, args2);
                va_list noArgs { };
                sCallback(*this, level, sFormatBuffer, noArgs);
            } else {
                // Not preformatted: pass the format string and va_list to the callback
                // (prefixing the object ref # if any):
                if (objRef) {
                    snprintf(sFormatBuffer, sizeof(sFormatBuffer), "{%s#%u} %s", obj.c_str(), objRef, fmt);
                    sCallback(*this, level, sFormatBuffer, args2);
                } else {
                    sCallback(*this, level, fmt, args2);
                }
            }
            va_end(args2);
        }

        // Write to the encoded log file:
        if (level >= sFileMinLevel) {
            dylog(level, _name, (LogEncoder::ObjectRef)objRef, fmt, args);
        }
    }


    void LogDomain::vlog(LogLevel level, const char *fmt, va_list args) {
        vlog(level, LogEncoder::None, true, fmt, args);
    }


    void LogDomain::log(LogLevel level, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, LogEncoder::None, true, fmt, args);
        va_end(args);
    }

    void LogDomain::vlogNoCallback(LogLevel level, const char *fmt, va_list args) {
        vlog(level, LogEncoder::None, false, fmt, args);
    }


    void LogDomain::logNoCallback(LogLevel level, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, LogEncoder::None, false, fmt, args);
        va_end(args);
    }

    void LogDomain::dylog(LogLevel level, const char* domain, unsigned objRef, const char *fmt, va_list args)
    {
        auto obj = getObject(objRef);

        if(sLogEncoder[(int)level]) {
            sLogEncoder[(int)level]->vlog(domain, sObjNames, (LogEncoder::ObjectRef)objRef, fmt, args);
        } else if(sFileOut[(int)level]) {
            static char formatBuffer[2048];
            size_t n = 0;
            LogDecoder::writeTimestamp(LogDecoder::now(), *sFileOut[(int)level]);
            LogDecoder::writeHeader(kLevels[(int)level], domain, *sFileOut[(int)level]);
            if (objRef)
                n = snprintf(formatBuffer, sizeof(formatBuffer), "{%s#%u} ", obj.c_str(), objRef);
            vsnprintf(&formatBuffer[n], sizeof(formatBuffer) - n, fmt, args);
            *sFileOut[(int)level] << formatBuffer << endl;
        } else {
            // No rotation if neither encoder nor file is present
            return;
        }

        const auto pos = sFileOut[(int)level]->tellp();
        if(pos >= sMaxSize) {
            Logging::rotateLog(level);
        }
    }
    
    static void invokeCallback(LogDomain &domain, LogLevel level, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        if (sCallbackPreformatted) {
            vsnprintf(sFormatBuffer, sizeof(sFormatBuffer), fmt, args);
            va_list noArgs { };
            sCallback(domain, level, sFormatBuffer, noArgs);
        } else {
            sCallback(domain, level, fmt, args);
        }
        va_end(args);
    }

    // The default logging callback writes to stderr, or on Android to __android_log_write.
    void LogDomain::defaultCallback(const LogDomain &domain, LogLevel level,
                                    const char *fmt, va_list args){
        #if ANDROID
            string tag("LiteCore");
            string domainName(domain.name());
            if (!domainName.empty())
                tag += " [" + domainName + "]";
            static const int androidLevels[5] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
                                                 ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                                 ANDROID_LOG_ERROR};
            __android_log_vprint(androidLevels[(int) level], tag.c_str(), fmt, args);
        #else
            auto name = domain.name();
            LogDecoder::writeTimestamp(LogDecoder::now(), cerr);
            LogDecoder::writeHeader(kLevels[(int)level], name, cerr);
            vfprintf(stderr, fmt, args);
            fputc('\n', stderr);
        #endif
    }

    // Must be called from a method holding sLogMutex
    string LogDomain::getObject(unsigned ref)
    {
        const auto found = sObjNames.find(ref);
        if(found != sObjNames.end()) {
            return found->second;
        }

        return "?";
    }


    unsigned LogDomain::registerObject(const void *object,
                                       const string &description,
                                       const string &nickname,
                                       LogLevel level)
    {
        unique_lock<mutex> lock(sLogMutex);
        unsigned objRef = ++slastObjRef;
        sObjNames.insert({objRef, nickname});
        if (sCallback && level >= _callbackLogLevel())
        invokeCallback(*this, level, "{%s#%u}==> %s @%p",
            nickname.c_str(), objRef, description.c_str(), object);

        return objRef;
    }

    void LogDomain::unregisterObject(unsigned objectRef) {
        unique_lock<mutex> lock(sLogMutex);
        sObjNames.erase(objectRef);
    }


#pragma mark - LOGGING CLASS:


    Logging::~Logging() {
        if (_objectRef)
            _domain.unregisterObject(_objectRef);
    }


    static std::string classNameOf(const Logging *obj) {
        const char *name = typeid(*obj).name();
#if defined(__clang__) && !defined(__ANDROID__)
        // Get the name of my class, unmangle it, and remove namespaces:
        size_t unmangledLen;
        int status;
        char *unmangled = abi::__cxa_demangle(name, nullptr, &unmangledLen, &status);
        if (unmangled)
            name = unmangled;
        string result(name);
        free(unmangled);
        return result;
#else
        return name;
#endif
    }

    std::string Logging::loggingClassName() const {
        string name = classNameOf(this);
        auto colon = name.find_last_of(':');
        if (colon != string::npos)
            name = name.substr(colon+1);
        return name;
    }
    

    std::string Logging::loggingIdentifier() const {
        return format("%p", this);
    }


    void Logging::_log(LogLevel level, const char *format, ...) const {
        va_list args;
        va_start(args, format);
        _logv(level, format, args);
        va_end(args);
    }
    
    void Logging::_logv(LogLevel level, const char *format, va_list args) const {
        _domain.computeLevel();
        if (!_domain.willLog(level))
            return;

        if(_objectRef == 0) {
            string nickname = loggingClassName();
            string identifier = classNameOf(this) + " " + loggingIdentifier();
            const_cast<Logging*>(this)->_objectRef = _domain.registerObject(this, identifier,
                                                                            nickname, level);
        }
        
        _domain.vlog(level, _objectRef, true, format, args);
    }


}
