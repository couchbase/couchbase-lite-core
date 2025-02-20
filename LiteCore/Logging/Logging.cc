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
#include "Logging_Internal.hh"
#include "LogObjectMap.hh"
#include "StringUtil.hh"
#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "LogObserver.hh"
#include "FilePath.hh"
#include "Error.hh"
#include "c4Base.h"  // for c4_now()
#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <ctime>

#if ( defined(__linux__) || defined(__APPLE__) ) && !defined(__ANDROID__)
#    include <cxxabi.h>
#endif

#ifdef _MSC_VER
#    include <winapifamily.h>
// For strcasecmp
#    include "PlatformIO.hh"
#endif

using namespace std;
using namespace std::chrono;
using namespace litecore::loginternal;

namespace litecore {

    namespace loginternal {
        LogObjectMap sObjectMap;
    }  // namespace loginternal

    atomic<LogDomain*> LogDomain::sFirstDomain = nullptr;

    static LogDomain ActorLog_("Actor");
    LogDomain&       ActorLog = ActorLog_;

#pragma mark - INITIALIZATION:

    LogDomain::LogDomain(const char* name, LogLevel level, bool internName)
        : _level(level), _name(internName ? strdup(name) : name), _observers(new LogObservers) {
        // Atomically add myself to the head of the list:
        LogDomain* first = sFirstDomain;
        do { _next = first; } while ( !sFirstDomain.compare_exchange_strong(first, this) );
    }

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
        // Setting "LiteCoreLog___" env var forces a minimum level:
        auto envLevel = levelFromEnvironment();
        if ( envLevel != LogLevel::Uninitialized ) level = min(level, envLevel);

        static mutex     sSetLevelMutex;
        unique_lock lock(sSetLevelMutex);
        _level = level;
        // The effective level is the level at which I will actually trigger because there is
        // a place for my output to go:
        _effectiveLevel = max(_level.load(), _observers->lowestLevel());
    }

    LogDomain* LogDomain::named(const char* name) {
        if ( !name ) name = "";
        for ( auto d = sFirstDomain.load(); d; d = d->_next )
            if ( strcmp(d->name(), name) == 0 ) return d;
        return nullptr;
    }

#pragma mark - LOGGING:

    void LogDomain::vlog(LogLevel level, const Logging* logger, bool doCallback, const char* fmt, va_list args) {
        if ( computeLevel() > level ) return;

        string      prefix;
        RawLogEntry entry{.timestamp = uint64_t(c4_now()),
                          .domain    = *this,
                          .level     = level,
                          .objRef    = LogObjectRef::None,
                          .prefix    = prefix,  // a reference to it
                          .fileOnly  = !doCallback};
        if ( logger ) {
            entry.objRef = logger->getObjectRef();
            prefix       = logger->loggingKeyValuePairs();
        }

        _observers->notify(entry, fmt, args);
    }

    void LogDomain::logToCallbacksOnly(LogLevel level, const char* message) {
        if ( computeLevel() > level ) return;
        _observers->notifyCallbacksOnly(
                LogEntry{.timestamp = uint64_t(c4_now()), .domain = *this, .level = level, .message = message});
    }

    void LogDomain::vlog(LogLevel level, const char* fmt, va_list args) { vlog(level, nullptr, true, fmt, args); }

    void LogDomain::log(LogLevel level, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, nullptr, true, fmt, args);
        va_end(args);
    }

    void LogDomain::logNoCallback(LogLevel level, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, nullptr, false, fmt, args);
        va_end(args);
    }

#pragma mark - LOGGING CLASS:

    Logging::~Logging() {
        if ( _objectRef != LogObjectRef::None ) sObjectMap.unregisterObject(_objectRef);
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

    LogObjectRef Logging::getObjectRef(LogLevel level) const {
        if ( _objectRef == LogObjectRef::None ) {
            string nickname   = loggingClassName();
            string identifier = classNameOf(this) + " " + loggingIdentifier();
            if ( sObjectMap.registerObject(&_objectRef, nickname) ) {
                // The binary logger will write a description of the object the first time it logs,
                // but callback loggers won't, so give them a special message to log:
                string message =
                        stringprintf("{%s#%u}==> %s @%p", nickname.c_str(), _objectRef, identifier.c_str(), this);
                _domain.logToCallbacksOnly(level, message.c_str());
            }
        }
        return _objectRef;
    }

    void Logging::setParentObjectRef(LogObjectRef parentObjRef) {
        Assert(sObjectMap.registerParentObject(getObjectRef(), parentObjRef));
    }

    void Logging::_logv(LogLevel level, const char* format, va_list args) const {
        if ( _domain.willLog(level) ) _domain.vlog(level, this, true, format, args);
    }
}  // namespace litecore
