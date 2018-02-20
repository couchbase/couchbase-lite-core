//
// Logging.hh
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

#pragma once
#include "slice.hh"
#include "PlatformCompat.hh"
#include <atomic>
#include <map>
#include <string>
#include <stdarg.h>
#include <stdint.h>

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

enum class LogLevel : int8_t {
    Uninitialized = -1,
    Debug,
    Verbose,
    Info,
    Warning,
    Error,
    None
};


class LogDomain {
public:
    LogDomain(const char *name, LogLevel level =LogLevel::Info)
    :_name(name),
     _level(level),
     _next(sFirstDomain)
    {
        sFirstDomain = this;
    }

    static LogDomain* named(const char *name);

    const char* name() const                        {return _name;}

    void setLevel(LogLevel lvl) noexcept;
    LogLevel level() const noexcept;

    /** The level at which this domain will actually have an effect. This is based on the level(),
        but raised to take into account the levels at which the callback and/or encoded file will
        trigger. In other words, any log() calls below this level will produce no output. */
    LogLevel effectiveLevel()                       {computeLevel(); return _effectiveLevel;}

    bool willLog(LogLevel lv) const                 {return _effectiveLevel <= lv;}

    void log(LogLevel level, const char *fmt, ...) __printflike(3, 4);
    void vlog(LogLevel level, const char *fmt, va_list);

    using Callback_t = void(*)(const LogDomain&, LogLevel, const char *format, va_list);

    static void defaultCallback(const LogDomain&, LogLevel, const char *format, va_list);

    /** Registers (or unregisters) a callback to be passed log messages.
        @param callback  The callback function, or NULL to unregister.
        @param preformatted  If true, callback will be passed already-formatted log messages to be
            displayed verbatim (and the `va_list` parameter will be NULL.) */
    static void setCallback(Callback_t callback, bool preformatted);

    /** Registers (or unregisters) a file to which log messages will be written in binary format.
        @param filePath  The file to write to, or an empty string to stop writing.
        @param atLevel  Only log messages at this or a higher level will be written.
        @param initialMessage  First message that will be written to the log, e.g. version info */
    static void writeEncodedLogsTo(const std::string &filePath,
                                   LogLevel atLevel,
                                   const std::string &initialMessage);

    static LogLevel callbackLogLevel() noexcept;
    static LogLevel fileLogLevel() noexcept             {return sFileMinLevel;}
    static void setCallbackLogLevel(LogLevel) noexcept;
    static void setFileLogLevel(LogLevel) noexcept;

private:
    friend class Logging;
    unsigned registerObject(const std::string &description,
                            const std::string &nickname,
                            LogLevel);
    void unregisterObject(unsigned obj);
    void vlog(LogLevel level, unsigned obj, const char *fmt, va_list);

private:
    static LogLevel _callbackLogLevel() noexcept;
    LogLevel computeLevel() noexcept;
    LogLevel levelFromEnvironment() const noexcept;
    static void _invalidateEffectiveLevels() noexcept;

    std::atomic<LogLevel> _effectiveLevel {LogLevel::Uninitialized};
    std::atomic<LogLevel> _level;
    const char* const _name;
    LogDomain* const _next;
    unsigned _lastObjRef {0};
    std::map<unsigned,std::string> _objNames;

    static LogDomain* sFirstDomain;
    static LogLevel sCallbackMinLevel;
    static LogLevel sFileMinLevel;
};

extern "C" LogDomain kC4Cpp_DefaultLog;
extern LogDomain DBLog, QueryLog, SyncLog;


#ifdef _MSC_VER
#define LogToAt(DOMAIN, LEVEL, FMT, ...) \
    {if (_usuallyFalse((DOMAIN).willLog(LogLevel::LEVEL))) \
        (DOMAIN).log(LogLevel::LEVEL, FMT, ##__VA_ARGS__);}

#define LogTo(DOMAIN, FMT, ...)         LogToAt(DOMAIN, Info, FMT, ##__VA_ARGS__)
#define LogVerbose(DOMAIN, FMT, ...)    LogToAt(DOMAIN, Verbose, FMT, ##__VA_ARGS__)
#define LogDebug(DOMAIN, FMT, ...)      LogToAt(DOMAIN, Debug, FMT, ##__VA_ARGS__)

#define Debug(FMT, ...)                 LogToAt(kC4Cpp_DefaultLog, Debug,   FMT, ##__VA_ARGS__)
#define Log(FMT, ...)                   LogToAt(kC4Cpp_DefaultLog, Info,    FMT, ##__VA_ARGS__)
#define Warn(FMT, ...)                  LogToAt(kC4Cpp_DefaultLog, Warning, FMT, ##__VA_ARGS__)
#define WarnError(FMT, ...)             LogToAt(kC4Cpp_DefaultLog, Error,   FMT, ##__VA_ARGS__)
#else
#define LogToAt(DOMAIN, LEVEL, FMT, ARGS...) \
    ({if (_usuallyFalse((DOMAIN).willLog(LogLevel::LEVEL))) \
        (DOMAIN).log(LogLevel::LEVEL, FMT, ##ARGS);})

#define LogTo(DOMAIN, FMT, ARGS...)         LogToAt(DOMAIN, Info, FMT, ##ARGS)
#define LogVerbose(DOMAIN, FMT, ARGS...)    LogToAt(DOMAIN, Verbose, FMT, ##ARGS)
#define LogDebug(DOMAIN, FMT, ARGS...)      LogToAt(DOMAIN, Debug, FMT, ##ARGS)

#define Debug(FMT, ARGS...)                 LogToAt(kC4Cpp_DefaultLog, Debug,   FMT, ##ARGS)
#define Log(FMT, ARGS...)                   LogToAt(kC4Cpp_DefaultLog, Info,    FMT, ##ARGS)
#define Warn(FMT, ARGS...)                  LogToAt(kC4Cpp_DefaultLog, Warning, FMT, ##ARGS)
#define WarnError(FMT, ARGS...)             LogToAt(kC4Cpp_DefaultLog, Error,   FMT, ##ARGS)
#endif


static inline bool WillLog(LogLevel lv)     {return kC4Cpp_DefaultLog.willLog(lv);}


// Debug(...) is stripped out of release builds
#if !DEBUG
    #undef Debug
    #ifdef _MSC_VER
        #define Debug(FMT, ...)
    #else
        #define Debug(FMT...)      ({ })
    #endif
#endif


    /** Mixin that adds log(), warn(), etc. methods. The messages these write will be prefixed
        with a description of the object; by default this is just the class and address, but
        you can customize it by overriding loggingIdentifier(). */
    class Logging {
    protected:
        Logging(LogDomain &domain)
        :_domain(domain)
        { }

        virtual ~Logging();

        /** Override this to return a string identifying this object. */
        virtual std::string loggingIdentifier() const;
        virtual std::string loggingClassName() const;

#if DEBUG
        // In debug mode, use code that's inefficient but allows use of __printflike, so the
        // compiler can catch invalid format parameters.
        #define LOGBODY(LEVEL)  va_list args; \
                                va_start(args, format); \
                                _logv(LogLevel::LEVEL, format, args); \
                                va_end(args);
        inline void log(const char *format, ...) const __printflike(2, 3)        {LOGBODY(Info)}
        inline void warn(const char *format, ...) const __printflike(2, 3)       {LOGBODY(Warning)}
        inline void logError(const char *format, ...) const __printflike(2, 3)   {LOGBODY(Error)}
        inline void logVerbose(const char *format, ...) const __printflike(2, 3) {LOGBODY(Verbose)}
        inline void logDebug(const char *format, ...) const __printflike(2, 3)   {LOGBODY(Debug)}

#else
        // In release mode, generate efficient code (but it can't be type-checked.)
        template <LogLevel LEVEL =LogLevel::Info, class... ARGS>
        inline void log(const char *format, ARGS... args) const {
            if (_usuallyFalse(_domain.willLog(LEVEL)))
                _log(LEVEL, format, args...);
        }

        template <class... ARGS>
        inline void warn(const char *format, ARGS... args) const {
            log<LogLevel::Warning>(format, args...);
        }
        template <class... ARGS>
        inline void logError(const char *format, ARGS... args) const {
            log<LogLevel::Error>(format, args...);
        }
        template <class... ARGS>
        inline void logVerbose(const char *format, ARGS... args) const {
            log<LogLevel::Verbose>(format, args...);
        }

        inline void logDebug(const char *format, ...) const {
            // does nothing in a release build
        }
#endif
        bool willLog(LogLevel level =LogLevel::Info) const         {return _domain.willLog(level);}

        void _log(LogLevel level, const char *format, ...) const;// __printflike(3, 4);
        void _logv(LogLevel level, const char *format, va_list) const;
        LogDomain &_domain;
        unsigned _objectRef {0};
    };

}
