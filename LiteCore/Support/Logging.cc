//
// Logging.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Logging.hh"
#include "StringUtil.hh"
#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "FilePath.hh"
#include "Error.hh"
#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <ctime>

#if __APPLE__
#    include <sys/time.h>
#endif
#if __ANDROID__
#    include <android/log.h>
#endif

#if ( defined(__linux__) || defined(__APPLE__) ) && !defined(__ANDROID__)
#    include <cxxabi.h>
#endif

#ifdef _MSC_VER
#    include <winapifamily.h>
// For strcasecmp
#    include "PlatformIO.hh"
#endif

#define CBL_LOG_EXTENSION ".cbllog"

using namespace std;
using namespace std::chrono;

struct ScopedSetter {
    bool& _var;
    bool  _origVal{};

    ScopedSetter(bool& var, bool toValue) : _var(var), _origVal(var) { _var = toValue; }

    ~ScopedSetter() { _var = _origVal; }
};

// Please don't use error::_throw. It may cause deadlock. Use this one.
#define ERROR_THROW(ERROR_CODE, FMT, ...)                                                                              \
    do {                                                                                                               \
        ScopedSetter setter(error::sWarnOnError, false);                                                               \
        error::_throw(ERROR_CODE, FMT, ##__VA_ARGS__);                                                                 \
    } while ( false )

namespace litecore {

    LogDomain*       LogDomain::sFirstDomain = nullptr;
    static LogDomain ActorLog_("Actor");
    LogDomain&       ActorLog = ActorLog_;

    LogLevel                     LogDomain::sCallbackMinLevel = LogLevel::Uninitialized;
    static LogDomain::Callback_t sCallback                    = LogDomain::defaultCallback;
    static bool                  sCallbackPreformatted        = false;
    LogLevel                     LogDomain::sFileMinLevel     = LogLevel::None;
    unsigned                     LogDomain::slastObjRef{0};
    LogDomain::ObjectMap         LogDomain::sObjectMap;
    static ofstream*             sFileOut[5]        = {};  // File per log level
    static LogEncoder*           sLogEncoder[5]     = {};
    static unsigned              sRotateSerialNo[5] = {};
    static LogFileOptions        sCurrentOptions;
    static string                sLogDirectory;
    static int                   sMaxCount = 0;     // For rotation
    static int64_t               sMaxSize  = 1024;  // For rotation
    static string                sInitialMessage;   // For rotation, goes at top of each log
    static unsigned              sWarningCount, sErrorCount;
    static mutex                 sLogMutex;
    std::vector<alloc_slice>     LogDomain::sInternedNames;

    static const char* const kLevelNames[] = {"debug", "verbose", "info", "warning", "error", nullptr};
    static const char*       kLevels[]     = {"Debug", "Verbose", "Info", "WARNING", "ERROR"};

    static string createLogPath(LogLevel level) {
        int64_t millisSinceEpoch = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        stringstream ss;
        ss << sLogDirectory << FilePath::kSeparator << "cbl_" << kLevelNames[(int)level] << "_" << millisSinceEpoch
           << CBL_LOG_EXTENSION;
        return ss.str();
    }
#ifdef LITECORE_CPPTEST
    string createLogPath_forUnitTest(LogLevel level) { return createLogPath(level); }

    void resetRotateSerialNo() {
        for ( auto& no : sRotateSerialNo ) { no = 0; }
    }
#endif

    static void setupFileOut() {
        for ( int i = 0; kLevelNames[i]; i++ ) {
            auto path   = createLogPath((LogLevel)i);
            sFileOut[i] = new ofstream(path, ofstream::out | ofstream::trunc | ofstream::binary);
            if ( !sFileOut[i]->good() ) {
                ERROR_THROW(error::LiteCoreError::CantOpenFile, "File Logger fails to open file, %s", path.c_str());
            }
        }
    }

    static void setupEncoders() {
        for ( int i = 0; i < 5; i++ ) { sLogEncoder[i] = new LogEncoder(*sFileOut[i], (LogLevel)i); }
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

    static bool needsTeardown(const LogFileOptions& options) {
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
           << "fileLogLevel=" << (int)LogDomain::fileLogLevel() << ","
           << "fileMaxSize=" << sMaxSize << ","
           << "fileMaxCount=" << sMaxCount;
        return ss.str();
    }

    void Logging::rotateLog(LogLevel level) {
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
            auto newEncoder         = new LogEncoder(*sFileOut[(int)level], level);
            sLogEncoder[(int)level] = newEncoder;
            newEncoder->log("", {}, LogEncoder::None, "---- %s ----", fileLogHeader(level).c_str());
            if ( !sInitialMessage.empty() ) {
                newEncoder->log("", {}, LogEncoder::None, "---- %s ----", sInitialMessage.c_str());
            }
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

    void LogDomain::flushLogFiles() {
        unique_lock<mutex> lock(sLogMutex);

        for ( auto& encoder : sLogEncoder )
            if ( encoder ) encoder->flush();
        for ( auto& fout : sFileOut )
            if ( fout ) fout->flush();
    }

#pragma mark - GLOBAL SETTINGS:

    void LogDomain::setCallback(Callback_t callback, bool preformatted) {
        unique_lock<mutex> lock(sLogMutex);
        if ( !callback ) sCallbackMinLevel = LogLevel::None;
        sCallback             = callback;
        sCallbackPreformatted = preformatted;
        _invalidateEffectiveLevels();
    }

    LogDomain::Callback_t LogDomain::currentCallback() { return sCallback; }

    void LogDomain::writeEncodedLogsTo(const LogFileOptions& options, const string& initialMessage) {
        unique_lock<mutex> lock(sLogMutex);
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
            if ( !teardown ) { return; }

            purgeOldLogs();
            setupFileOut();
            if ( !options.isPlaintext ) { setupEncoders(); }

            int8_t level = 0;
            if ( sLogEncoder[0] ) {
                for ( auto& encoder : sLogEncoder ) {
                    encoder->log("", {}, LogEncoder::None, "---- %s ----", fileLogHeader(LogLevel{level++}).c_str());
                    if ( !sInitialMessage.empty() ) {
                        encoder->log("", {}, LogEncoder::None, "---- %s ----", sInitialMessage.c_str());
                    }
                    encoder->flush();  // Make sure at least the magic bytes are present
                }
            } else {
                for ( auto& fout : sFileOut ) {
                    LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                    LogDecoder::writeHeader(kLevels[(int)level], "", *fout);
                    *fout << "---- " << fileLogHeader(LogLevel{level}) << " ----" << endl;
                    if ( !sInitialMessage.empty() ) {
                        LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                        LogDecoder::writeHeader(kLevels[(int)level], "", *fout);
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
                            for ( auto& encoder : sLogEncoder ) {
                                encoder->log("", {}, LogEncoder::None, "---- END ----");
                            }
                        } else if ( sFileOut[0] ) {
                            int8_t level = 0;
                            for ( auto& fout : sFileOut ) {
                                LogDecoder::writeTimestamp(LogDecoder::now(), *fout, true);
                                LogDecoder::writeHeader(kLevels[(int)level++], "", *fout);
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
        _invalidateEffectiveLevels();
        sCurrentOptions = options;
    }

    LogFileOptions LogDomain::currentLogFileOptions() {
        unique_lock<mutex> lock(sLogMutex);
        return sCurrentOptions;
    }

    void LogDomain::setCallbackLogLevel(LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);

        // Setting "LiteCoreLog" env var forces a minimum level of logging:
        auto envLevel = kC4Cpp_DefaultLog.levelFromEnvironment();
        if ( envLevel != LogLevel::Uninitialized ) level = min(level, envLevel);

        if ( level != sCallbackMinLevel ) {
            sCallbackMinLevel = level;
            _invalidateEffectiveLevels();
        }
    }

    void LogDomain::setFileLogLevel(LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);
        if ( level != sFileMinLevel ) {
            sFileMinLevel = level;
            _invalidateEffectiveLevels();
        }
    }

    // Only call while holding sLogMutex!
    void LogDomain::_invalidateEffectiveLevels() noexcept {
        for ( auto d = sFirstDomain; d; d = d->_next ) d->_effectiveLevel = LogLevel::Uninitialized;
    }

    LogLevel LogDomain::callbackLogLevel() noexcept {
        unique_lock<mutex> lock(sLogMutex);
        return _callbackLogLevel();
    }

    // Only call while holding sLogMutex!
    LogLevel LogDomain::_callbackLogLevel() noexcept {
        auto level = sCallbackMinLevel;
        if ( level == LogLevel::Uninitialized ) {
            // Allow 'LiteCoreLog' env var to set initial callback level:
            level = kC4Cpp_DefaultLog.levelFromEnvironment();
            if ( level == LogLevel::Uninitialized ) level = LogLevel::Info;
            sCallbackMinLevel = level;
        }
        return level;
    }

#pragma mark - INITIALIZATION:

    // Returns the LogLevel override set by an environment variable, or Uninitialized if none
    LogLevel LogDomain::levelFromEnvironment() const noexcept {
        char* val = getenv((string("LiteCoreLog") + _name).c_str());
        if ( val ) {
            static const char* const kEnvLevelNames[] = {"debug", "verbose", "info", "warning",
                                                         "error", "none",    nullptr};
            for ( int i = 0; kEnvLevelNames[i]; i++ ) {
                if ( 0 == strcasecmp(val, kEnvLevelNames[i]) ) return LogLevel(i);
            }
            return LogLevel::Info;
        }

        return LogLevel::Uninitialized;
    }

    LogLevel LogDomain::computeLevel() noexcept {
        if ( _effectiveLevel == LogLevel::Uninitialized ) setLevel(_level);
        return _level;
    }

    LogLevel LogDomain::level() const noexcept { return const_cast<LogDomain*>(this)->computeLevel(); }

    void LogDomain::setLevel(litecore::LogLevel level) noexcept {
        unique_lock<mutex> lock(sLogMutex);

        // Setting "LiteCoreLog___" env var forces a minimum level:
        auto envLevel = levelFromEnvironment();
        if ( envLevel != LogLevel::Uninitialized ) level = min(level, envLevel);

        _level = level;
        // The effective level is the level at which I will actually trigger because there is
        // a place for my output to go:
        _effectiveLevel = max((LogLevel)_level, min(_callbackLogLevel(), sFileMinLevel));
    }

    LogDomain* LogDomain::named(const char* name) {
        unique_lock<mutex> lock(sLogMutex);
        if ( !name ) name = "";
        for ( auto d = sFirstDomain; d; d = d->_next )
            if ( strcmp(d->name(), name) == 0 ) return d;
        return nullptr;
    }

#pragma mark - LOGGING:


    static char sFormatBuffer[2048];

    /*static*/ inline size_t LogDomain::addObjectPath(char* destBuf, size_t bufSize, unsigned obj) {
        auto objPath = getObjectPath(obj);
        return snprintf(destBuf, bufSize, "Obj=%s ", objPath.c_str());
    }

    void LogDomain::vlog(LogLevel level, const Logging* logger, bool doCallback, const char* fmt, va_list args) {
        if ( _effectiveLevel == LogLevel::Uninitialized ) computeLevel();
        if ( !willLog(level) ) return;

        unsigned          objRef{LogEncoder::None};
        std::stringstream prefixOut;
        if ( logger ) {
            objRef = logger->getObjectRef();
            logger->addLoggingKeyValuePairs(prefixOut);
        }
        const char* uncheckedFmt = fmt;
        std::string prefix       = prefixOut.str();
        std::string prefixedFmt;
        if ( !prefix.empty() ) {
            DebugAssert(prefix.find('%') == std::string::npos);
            prefixOut << " " << fmt;
            prefixedFmt  = prefixOut.str();
            uncheckedFmt = prefixedFmt.c_str();
        }

        unique_lock<mutex> lock(sLogMutex);

        // Invoke the client callback:
        if ( doCallback && sCallback && level >= _callbackLogLevel() ) {
            va_list args2;
            va_copy(args2, args);
            va_list noArgs{};

            // Argument fmt is checked by __printflike(5, 0); it is guaranteed a literal constant.
            // Here, we want to add a prefix to the format. This prefix is assigned by
            // logger. It is generated from the LiteCore source, Logging::addLoggingKeyValuePairs,
            // and we can trust that it does not include format specifier, '%'.
            // We will pass down uncheckedFmt by ignoring "-Wformat-nonliteral."
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

            size_t      n      = 0;
            const char* useFmt = uncheckedFmt;
            if ( objRef ) n = addObjectPath(sFormatBuffer, sizeof(sFormatBuffer), objRef);
            if ( sCallbackPreformatted ) {
                vsnprintf(&sFormatBuffer[n], sizeof(sFormatBuffer) - n, uncheckedFmt, args2);
                useFmt = sFormatBuffer;
            } else if ( n > 0 ) {
                snprintf(&sFormatBuffer[n], sizeof(sFormatBuffer) - n, "%s ", uncheckedFmt);
                useFmt = sFormatBuffer;
            }
            sCallback(*this, level, useFmt, sCallbackPreformatted ? noArgs : args2);

#pragma GCC diagnostic pop

            va_end(args2);
        }

        // Write to the encoded log file:
        if ( level >= sFileMinLevel ) { dylog(level, _name, (LogEncoder::ObjectRef)objRef, prefix, fmt, args); }
    }

    void LogDomain::vlog(LogLevel level, const char* fmt, va_list args) { vlog(level, nullptr, true, fmt, args); }

    void LogDomain::log(LogLevel level, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, nullptr, true, fmt, args);
        va_end(args);
    }

    void LogDomain::vlogNoCallback(LogLevel level, const char* fmt, va_list args) {
        vlog(level, nullptr, false, fmt, args);
    }

    void LogDomain::logNoCallback(LogLevel level, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, nullptr, false, fmt, args);
        va_end(args);
    }

    // Can't make this function static, it breaks the usage of __printflike.
    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    // Must have sLogMutex held
    void LogDomain::dylog(LogLevel level, const char* domain, unsigned objRef, const std::string& prefix,
                          const char* fmt, va_list args) {
        auto     obj = getObject(objRef);
        uint64_t pos;

        // Safe to store these in variables, since they only change in the rotateLog method
        // and the rotateLog method is only called here (and this method holds a mutex)
        const auto encoder = sLogEncoder[(int)level];
        const auto file    = sFileOut[(int)level];
        if ( encoder ) {
            encoder->vlog(domain, sObjectMap, (LogEncoder::ObjectRef)objRef, prefix, fmt, args);
            pos = encoder->tellp();
        } else if ( file ) {
            static char formatBuffer[2048];
            size_t      n = 0;
            LogDecoder::writeTimestamp(LogDecoder::now(), *file, true);
            LogDecoder::writeHeader(kLevels[(int)level], domain, *file);
            if ( objRef ) n = addObjectPath(formatBuffer, sizeof(formatBuffer), objRef);
            if ( prefix.empty() ) vsnprintf(&formatBuffer[n], sizeof(formatBuffer) - n, fmt, args);
            else {
                std::string prefixedFmt = stringprintf("%s %s", prefix.c_str(), fmt);
                // we pass unchecked prefixedFmt to following function.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

                vsnprintf(&formatBuffer[n], sizeof(formatBuffer) - n, prefixedFmt.c_str(), args);

#pragma GCC diagnostic pop
            }
            *file << formatBuffer << endl;
            pos = file->tellp();
        } else {
            // No rotation if neither encoder nor file is present
            return;
        }

        if ( pos >= sMaxSize ) { Logging::rotateLog(level); }

        if ( level == LogLevel::Warning ) sWarningCount++;
        else if ( level == LogLevel::Error )
            sErrorCount++;
    }

    // NOLINTEND(readability-convert-member-functions-to-static)

    __printflike(3, 4) static void invokeCallback(LogDomain& domain, LogLevel level, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        if ( sCallbackPreformatted ) {
            vsnprintf(sFormatBuffer, sizeof(sFormatBuffer), fmt, args);
            va_list noArgs{};
            sCallback(domain, level, sFormatBuffer, noArgs);
        } else {
            sCallback(domain, level, fmt, args);
        }
        va_end(args);
    }

    // The default logging callback writes to stderr, or on Android to __android_log_write.
    void LogDomain::defaultCallback(const LogDomain& domain, LogLevel level, const char* fmt, va_list args) {
#if ANDROID
        string tag("LiteCore");
        string domainName(domain.name());
        if ( !domainName.empty() ) tag += " [" + domainName + "]";
        static const int androidLevels[5] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                             ANDROID_LOG_ERROR};
        __android_log_vprint(androidLevels[(int)level], tag.c_str(), fmt, args);
#else
        auto name = domain.name();
        LogDecoder::writeTimestamp(LogDecoder::now(), cerr);
        LogDecoder::writeHeader(kLevels[(int)level], name, cerr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
#endif
    }

    // Must be called from a method holding sLogMutex
    string LogDomain::getObject(unsigned ref) {
        const auto found = sObjectMap.find(ref);
        if ( found != sObjectMap.end() ) { return found->second.first; }

        return "?";
    }

    static void getObjectPathRecur(const LogDomain::ObjectMap& objMap, LogDomain::ObjectMap::const_iterator iter,
                                   std::stringstream& ss) {
        // pre-conditions: iter != objMap.end()
        if ( iter->second.second != 0 ) {
            auto parentIter = objMap.find(iter->second.second);
            if ( parentIter == objMap.end() ) {
                // the parent object is deleted. We omit the loggingClassName
                ss << "/#" << iter->second.second;
            } else {
                getObjectPathRecur(objMap, parentIter, ss);
            }
        }
        ss << "/" << iter->second.first << "#" << iter->first;
    }

    std::string LogDomain::getObjectPath(unsigned obj, const ObjectMap& objMap) {
        auto iter = objMap.find(obj);
        if ( iter == objMap.end() ) { return ""; }
        std::stringstream ss;
        getObjectPathRecur(objMap, iter, ss);
        return ss.str() + "/";
    }

    unsigned LogDomain::registerObject(const void* object, const unsigned* val, const string& description,
                                       const string& nickname, LogLevel level) {
        unique_lock<mutex> lock(sLogMutex);
        if ( *val != 0 ) { return *val; }

        unsigned objRef = ++slastObjRef;
        sObjectMap.emplace(std::piecewise_construct, std::forward_as_tuple(objRef), std::forward_as_tuple(nickname, 0));
        if ( sCallback && level >= _callbackLogLevel() )
            invokeCallback(*this, level, "{%s#%u}==> %s @%p", nickname.c_str(), objRef, description.c_str(), object);
        return objRef;
    }

    void LogDomain::unregisterObject(unsigned objectRef) {
        unique_lock<mutex> lock(sLogMutex);
        sObjectMap.erase(objectRef);
    }

    bool LogDomain::registerParentObject(unsigned object, unsigned parentObject) {
        enum { kNoWarning, kNotRegistered, kParentNotRegistered, kAlreadyRegistered } warningCode{kNoWarning};

        {
            unique_lock<mutex> lock(sLogMutex);
            auto               iter = sObjectMap.find(object);
            if ( iter == sObjectMap.end() ) {
                warningCode = kNotRegistered;
            } else if ( sObjectMap.find(parentObject) == sObjectMap.end() ) {
                warningCode = kParentNotRegistered;
            } else if ( iter->second.second != 0 ) {
                warningCode = kAlreadyRegistered;
            } else
                iter->second.second = parentObject;
        }

        switch ( warningCode ) {
            case kNotRegistered:
                WarnError("LogDomain::registerParentObject, object is not registered");
                break;
            case kParentNotRegistered:
                WarnError("LogDomain::registerParentObject, parentObject is not registered");
                break;
            case kAlreadyRegistered:
                WarnError("LogDomain::registerParentObject, object is already assigned parent");
                break;
            default:
                break;
        }

        return warningCode == kNoWarning;
    }

#pragma mark - LOGGING CLASS:

    Logging::~Logging() {
        if ( _objectRef ) _domain.unregisterObject(_objectRef);
    }

    static std::string classNameOf(const Logging* obj) {
        const char* name = typeid(*obj).name();
#if ( defined(__linux__) || defined(__APPLE__) ) && !defined(__ANDROID__)
        // Get the name of my class, unmangle it, and remove namespaces:
        size_t unmangledLen;
        int    status;
        char*  unmangled = abi::__cxa_demangle(name, nullptr, &unmangledLen, &status);
        if ( unmangled ) name = unmangled;
        string result(name);
        free(unmangled);
        return result;
#else
        return name;
#endif
    }

    std::string Logging::loggingName() const {
        return stringprintf("%s#%u", loggingClassName().c_str(), getObjectRef());
    }

    std::string Logging::loggingClassName() const {
        string name  = classNameOf(this);
        auto   colon = name.find_last_of(':');
        if ( colon != string::npos ) name = name.substr(colon + 1);
        return name;
    }

    std::string Logging::loggingIdentifier() const { return stringprintf("%p", this); }

    unsigned Logging::getObjectRef(LogLevel level) const {
        if ( _objectRef == 0 ) {
            string nickname   = loggingClassName();
            string identifier = classNameOf(this) + " " + loggingIdentifier();
            _objectRef        = _domain.registerObject(this, &_objectRef, identifier, nickname, level);
        }
        return _objectRef;
    }

    void Logging::setParentObjectRef(unsigned parentObjRef) {
        Assert(_domain.registerParentObject(getObjectRef(), parentObjRef));
    }

    void Logging::_log(LogLevel level, const char* format, ...) const {
        va_list args;
        va_start(args, format);
        _logv(level, format, args);
        va_end(args);
    }

    void Logging::_logv(LogLevel level, const char* format, va_list args) const {
        _domain.computeLevel();
        if ( _domain.willLog(level) ) _domain.vlog(level, this, true, format, args);
    }
}  // namespace litecore
