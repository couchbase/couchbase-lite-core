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
#include "c4Compat.h"
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cinttypes>  //for stdint.h fmt specifiers
#include <string>

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

    enum class LogLevel : int8_t { Uninitialized = -1, Debug, Verbose, Info, Warning, Error, None };

    static constexpr size_t kNumLogLevels = 5;  ///< Number of active levels, Debug...Error

    enum class LogObjectRef : unsigned { None = 0 };

    class Logging;
    class LogObservers;

    class LogDomain {
      public:
        explicit LogDomain(const char* name, LogLevel level = LogLevel::Info, bool internName = false);

        static LogDomain* named(const char* name);

        const char* name() const { return _name; }

        void     setLevel(LogLevel lvl) noexcept;
        LogLevel level() const noexcept;

        /// The first domain in the linked list (in arbitrary order.)
        static LogDomain* first() noexcept { return sFirstDomain; }

        /// The next domain in the linked list (in arbitrary order), or nullptr at the end.
        LogDomain* next() const noexcept { return _next; }

        /// The level at which this domain will actually have an effect. This is based on the level(),
        /// but raised to take into account the levels of LogObservers.
        /// In other words, any log() calls below this level will produce no output. */
        LogLevel effectiveLevel() {
            computeLevel();
            return _effectiveLevel;
        }

        bool willLog(LogLevel lv) const { return _effectiveLevel <= lv; }

        void logNoCallback(LogLevel level, const char* fmt, ...) __printflike(3, 4);
        void log(LogLevel level, const char* fmt, ...) __printflike(3, 4);
        void vlog(LogLevel level, const char* fmt, va_list) __printflike(3, 0);

      private:
        friend class Logging;
        friend class LogObserver;

        void vlog(LogLevel level, const Logging* logger, bool callback, const char* fmt, va_list) __printflike(5, 0);
        void logToCallbacksOnly(LogLevel, const char* message);

        LogLevel computeLevel() noexcept;
        LogLevel levelFromEnvironment() const noexcept;

        void invalidateLevel() noexcept { _effectiveLevel = LogLevel::Uninitialized; }

        std::atomic<LogLevel> _effectiveLevel{LogLevel::Uninitialized};
        std::atomic<LogLevel> _level;
        const char*           _name;
        LogDomain*            _next;
        LogObservers*         _observers;

        static LogDomain* sFirstDomain;
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

    /** Mixin that adds log(), warn(), etc. methods. The messages these write will be prefixed
        with a description of the object; by default this is just the class and address, but
        you can customize it by overriding loggingIdentifier(). */
    class Logging {
      public:
        std::string loggingName() const;

        LogObjectRef getObjectRef(LogLevel level = LogLevel::Info) const;
        void         setParentObjectRef(LogObjectRef parentObjRef);

      protected:
        explicit Logging(LogDomain& domain) : _domain(domain) {}

        virtual ~Logging();

        /** Override this to return a string identifying this object. */
        virtual std::string loggingIdentifier() const;
        virtual std::string loggingClassName() const;

        /** Override this to return aditional metadata about the object, in the form of
            space-separated "key=value" pairs.
            These will be logged with every message, even in the binary log file. */
        virtual std::string loggingKeyValuePairs() const { return {}; }

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

        LogDomain& _domain;

      private:
        friend class LogDomain;

        mutable LogObjectRef _objectRef{};
    };

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
