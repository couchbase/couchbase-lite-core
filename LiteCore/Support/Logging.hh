//
// Logging.hh
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
#include "fleece/slice.hh"
#include "c4Compat.h"
#include <atomic>
#include <map>
#include <string>
#include <cstdarg>
#include <cstdint>
#include <cinttypes>  //for stdint.h fmt specifiers
#include <vector>

/*
    This is a configurable console-logging facility that lets logging be turned on and off independently for various subsystems or areas of the code. It's used similarly to printf:
        Log(@"the value of foo is %d", foo);

    You can associate a log message with a particular subsystem or tag by defining a logging domain. In one source file, define the domain:
        DefineLogDomain(Foo);
    If you need to use the same domain in other source files, add the line
        UsingLogDomain(Foo);
    Now you can use the Foo domain for logging:
        LogTo(Foo, @"the value of foo is %d", foo);
 
    By default, logging is compiled in but disabled at runtime.

    To enable logging in general, set the user default 'Log' to 'YES'. You can do this persistently using the 'defaults write' command; but it's very convenient during development to use the Arguments tab in the Xcode Executable Info panel. Just add a new entry to the arguments list, of the form "-Log YES". Now you can check and uncheck that as desired; the change will take effect when relaunching.

    Once logging is enabled, you can turn on individual domains. For any domain "Foo", to enable output from calls of the form LogTo(Foo, @"..."), set the user default 'LogFoo' to 'YES', just as above.
 
    You can use LogVerbose() and LogDebug() for messages that add more detail but shouldn't be seen by default when the domain is enabled. To enable verbose mode for a domain, e.g. 'Foo', set the default 'LogFooVerbose' to YES. To enable both verbose and debug modes, set 'LogFooDebug' to YES.

    Warn() is a related function that _always_ logs, and prefixes the message with "WARNING:".
        Warn(@"Reactor coolant system has failed");
 
    Note: Logging is still present in release/nondebug builds. I've found this to be very useful in tracking down problems in the field, since I can tell a user how to turn on logging, and then get detailed logs back. To disable logging code from being compiled at all, define the preprocessor symbol _DISABLE_LOGGING (in your prefix header or target build settings.)
*/

namespace litecore {

    using namespace fleece;

    enum class LogLevel : int8_t { Uninitialized = -1, Debug, Verbose, Info, Warning, Error, None };

    struct LogFileOptions {
        std::string path;
        LogLevel    level;
        int64_t     maxSize;
        int         maxCount;
        bool        isPlaintext;
    };

    class Logging;

    class LogDomain {
      public:
        // objectRef -> (loggingName, parentObectRef)
        using ObjectMap = std::map<unsigned, std::pair<std::string, unsigned>>;

        explicit LogDomain(const char* name, LogLevel level = LogLevel::Info, bool internName = false)
            : _level(level), _name(name), _next(sFirstDomain) {
            sFirstDomain = this;
            if ( internName ) {
                slice nslice{_name};
                sInternedNames.push_back(alloc_slice::nullPaddedString(nslice));
                _name = (const char*)sInternedNames.back().buf;
            }
        }

        static LogDomain* named(const char* name);

        const char* name() const { return _name; }

        void     setLevel(LogLevel lvl) noexcept;
        LogLevel level() const noexcept;

        /** The level at which this domain will actually have an effect. This is based on the level(),
        but raised to take into account the levels at which the callback and/or encoded file will
        trigger. In other words, any log() calls below this level will produce no output. */
        LogLevel effectiveLevel() {
            computeLevel();
            return _effectiveLevel;
        }

        bool willLog(LogLevel lv) const { return _effectiveLevel <= lv; }

        void logNoCallback(LogLevel level, const char* fmt, ...) __printflike(3, 4);
        void log(LogLevel level, const char* fmt, ...) __printflike(3, 4);
        void vlog(LogLevel level, const char* fmt, va_list) __printflike(3, 0);
        void vlogNoCallback(LogLevel level, const char* fmt, va_list) __printflike(3, 0);

        using Callback_t = void (*)(const LogDomain&, LogLevel, const char* format, va_list);

        static void       defaultCallback(const LogDomain&, LogLevel, const char* format, va_list) __printflike(3, 0);
        static Callback_t currentCallback();

        /** Registers (or unregisters) a callback to be passed log messages.
        @param callback  The callback function, or NULL to unregister.
        @param preformatted  If true, callback will be passed already-formatted log messages to be
            displayed verbatim (and the `va_list` parameter will be NULL.) */
        static void setCallback(Callback_t callback, bool preformatted);

        /** Registers (or unregisters) a file to which log messages will be written in binary format.
        @param options The options to use when performing file logging
        @param initialMessage  First message that will be written to the log, e.g. version info */
        static void writeEncodedLogsTo(const LogFileOptions& options, const std::string& initialMessage = "");

        /** Returns the current log file configuration options, as given to `writeEncodedLogsTo`. */
        static LogFileOptions currentLogFileOptions();

        static LogLevel callbackLogLevel() noexcept;

        static LogLevel fileLogLevel() noexcept { return sFileMinLevel; }

        static void setCallbackLogLevel(LogLevel) noexcept;
        static void setFileLogLevel(LogLevel) noexcept;

        static void flushLogFiles();

        static unsigned warningCount();  ///< Number of warnings logged since launch
        static unsigned errorCount();    ///< Number of errors logged since launch

        static std::string getObjectPath(unsigned obj, const ObjectMap& objMap);

      private:
        friend class Logging;
        static std::string getObject(unsigned);
        unsigned           registerObject(const void* object, const unsigned* val, const std::string& description,
                                          const std::string& nickname, LogLevel level);
        static bool        registerParentObject(unsigned object, unsigned parentObject);
        static void        unregisterObject(unsigned obj);

        static std::string getObjectPath(unsigned obj) { return getObjectPath(obj, sObjectMap); }

        static inline size_t addObjectPath(char* destBuf, size_t bufSize, unsigned obj);
        void vlog(LogLevel level, const Logging* logger, bool callback, const char* fmt, va_list) __printflike(5, 0);

      private:
        static LogLevel _callbackLogLevel() noexcept;
        LogLevel        computeLevel() noexcept;
        LogLevel        levelFromEnvironment() const noexcept;
        static void     _invalidateEffectiveLevels() noexcept;

        static void dylog(LogLevel level, const char* domain, unsigned objRef, const std::string& prefix,
                          const char* fmt, va_list) __printflike(5, 0);

        std::atomic<LogLevel> _effectiveLevel{LogLevel::Uninitialized};
        std::atomic<LogLevel> _level;
        const char*           _name;
        LogDomain* const      _next;

        static unsigned                 slastObjRef;
        static ObjectMap                sObjectMap;
        static LogDomain*               sFirstDomain;
        static LogLevel                 sCallbackMinLevel;
        static LogLevel                 sFileMinLevel;
        static std::vector<alloc_slice> sInternedNames;
    };

    extern "C" CBL_CORE_API LogDomain kC4Cpp_DefaultLog;
    extern LogDomain                  BlobLog, DBLog, QueryLog, SyncLog, &ActorLog;


#define LogToAt(DOMAIN, LEVEL, FMT, ...)                                                                               \
    do {                                                                                                               \
        if ( _usuallyFalse((DOMAIN).willLog(litecore::LogLevel::LEVEL)) )                                              \
            (DOMAIN).log(litecore::LogLevel::LEVEL, FMT, ##__VA_ARGS__);                                               \
    } while ( 0 )

#define LogTo(DOMAIN, FMT, ...)      LogToAt(DOMAIN, Info, FMT, ##__VA_ARGS__)
#define LogVerbose(DOMAIN, FMT, ...) LogToAt(DOMAIN, Verbose, FMT, ##__VA_ARGS__)
#define LogWarn(DOMAIN, FMT, ...)    LogToAt(DOMAIN, Warning, FMT, ##__VA_ARGS__)
#define LogError(DOMAIN, FMT, ...)   LogToAt(DOMAIN, Error, FMT, ##__VA_ARGS__)

#define Log(FMT, ...)       LogToAt(litecore::kC4Cpp_DefaultLog, Info, FMT, ##__VA_ARGS__)
#define Warn(FMT, ...)      LogToAt(litecore::kC4Cpp_DefaultLog, Warning, FMT, ##__VA_ARGS__)
#define WarnError(FMT, ...) LogToAt(litecore::kC4Cpp_DefaultLog, Error, FMT, ##__VA_ARGS__)

#ifdef DEBUG
#    define LogDebug(DOMAIN, FMT, ...) LogToAt(DOMAIN, Debug, FMT, ##__VA_ARGS__)
#    define WriteDebug(FMT, ...)       LogToAt(litecore::kC4Cpp_DefaultLog, Debug, FMT, ##__VA_ARGS__)
#else
#    define LogDebug(DOMAIN, FMT, ...)
#    define WriteDebug(FMT, ...)
#endif

    inline bool WillLog(LogLevel lv) { return kC4Cpp_DefaultLog.willLog(lv); }

    /** Mixin that adds log(), warn(), etc. methods. The messages these write will be prefixed
        with a description of the object; by default this is just the class and address, but
        you can customize it by overriding loggingIdentifier(). */
    class Logging {
      public:
        std::string loggingName() const;

        unsigned getObjectRef(LogLevel level = LogLevel::Info) const;
        void     setParentObjectRef(unsigned parentObjRef);

      protected:
        explicit Logging(LogDomain& domain) : _domain(domain) {}

        virtual ~Logging();

        /** Override this to return a string identifying this object. */
        virtual std::string loggingIdentifier() const;
        virtual std::string loggingClassName() const;

#define LOGBODY_(LEVEL)                                                                                                \
    va_list args;                                                                                                      \
    va_start(args, format);                                                                                            \
    _logv(LEVEL, format, args);                                                                                        \
    va_end(args);
#define LOGBODY(LEVEL) LOGBODY_(LogLevel::LEVEL)

        void warn(const char* format, ...) const __printflike(2, 3) { LOGBODY(Warning) }

        void logError(const char* format, ...) const __printflike(2, 3) { LOGBODY(Error) }

        // For performance reasons, logInfo(), logVerbose(), logDebug() are macros (below)
        void _logInfo(const char* format, ...) const __printflike(2, 3) { LOGBODY(Info) }

        void _logVerbose(const char* format, ...) const __printflike(2, 3) { LOGBODY(Verbose) }

        void _logDebug(const char* format, ...) const __printflike(2, 3) { LOGBODY(Debug) }

        bool willLog(LogLevel level = LogLevel::Info) const { return _domain.willLog(level); }

        void _log(LogLevel level, const char* format, ...) const __printflike(3, 4) { LOGBODY_(level) }

        void _logv(LogLevel level, const char* format, va_list) const __printflike(3, 0);

        // Add key=value pairs to the output. They are space separated. If output is not empty
        // upon entry, add a space to start new key=value pairs.
        // Warning: the string must not include printf format specifier, '%'.
        virtual void addLoggingKeyValuePairs(std::stringstream& output) const {}

        LogDomain& _domain;

      private:
        friend class LogDomain;
        static void rotateLog(LogLevel level);

        mutable unsigned _objectRef{0};
    };

#ifdef LITECORE_CPPTEST
    std::string createLogPath_forUnitTest(LogLevel level);
    void        resetRotateSerialNo();
#endif

#define _logAt(LEVEL, FMT, ...)                                                                                        \
    do {                                                                                                               \
        if ( _usuallyFalse(this->willLog(litecore::LogLevel::LEVEL)) )                                                 \
            this->_log(litecore::LogLevel::LEVEL, FMT, ##__VA_ARGS__);                                                 \
    } while ( 0 )
#define logInfo(FMT, ...)    _logAt(Info, FMT, ##__VA_ARGS__)
#define logVerbose(FMT, ...) _logAt(Verbose, FMT, ##__VA_ARGS__)

#if DEBUG
#    define logDebug(FMT, ...) _logAt(Debug, FMT, ##__VA_ARGS__)
#else
#    define logDebug(FMT, ...)
#endif

}  // namespace litecore
