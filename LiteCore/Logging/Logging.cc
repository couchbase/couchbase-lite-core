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
#include "StringUtil.hh"
#include "LogFiles.hh"  //TODO: remove
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

using namespace std;
using namespace std::chrono;
using namespace litecore::loginternal;

namespace litecore {

    namespace loginternal {
        mutex sLogMutex;
        char  sFormatBuffer[2048];
    }  // namespace loginternal

    LogDomain* LogDomain::sFirstDomain = nullptr;

    static LogDomain ActorLog_("Actor");
    LogDomain&       ActorLog = ActorLog_;

#pragma mark - INITIALIZATION:

    LogDomain::LogDomain(const char* name, LogLevel level, bool internName)
        : _level(level), _name(internName ? strdup(name) : name), _observers(new LogObservers) {
        // First-time initialization:
        static once_flag sOnce;
        call_once(sOnce, [] { LogCallback::setCallback(LogCallback::defaultCallback, false); });

        unique_lock<mutex> lock(sLogMutex);
        _next        = sFirstDomain;
        sFirstDomain = this;
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
        unique_lock<mutex> lock(sLogMutex);

        // Setting "LiteCoreLog___" env var forces a minimum level:
        auto envLevel = levelFromEnvironment();
        if ( envLevel != LogLevel::Uninitialized ) level = min(level, envLevel);

        _level = level;
        // The effective level is the level at which I will actually trigger because there is
        // a place for my output to go:
        _effectiveLevel = max(_level.load(), _observers->lowestLevel());
    }

    LogDomain* LogDomain::named(const char* name) {
        unique_lock<mutex> lock(sLogMutex);
        if ( !name ) name = "";
        for ( auto d = sFirstDomain; d; d = d->_next )
            if ( strcmp(d->name(), name) == 0 ) return d;
        return nullptr;
    }

#pragma mark - LOGGING:

    void LogDomain::vlog(LogLevel level, const Logging* logger, bool doCallback, const char* fmt, va_list args) {
        if ( _effectiveLevel == LogLevel::Uninitialized ) computeLevel();
        if ( !willLog(level) ) return;

        auto         timestamp = uint64_t(c4_now());
        LogObjectRef objRef{LogEncoder::None};

        const char* uncheckedFmt = fmt;
        std::string prefix, prefixedFmt;
        if ( logger ) {
            objRef = logger->getObjectRef();
            std::stringstream prefixOut;
            logger->addLoggingKeyValuePairs(prefixOut);
            prefix = prefixOut.str();
            if ( !prefix.empty() ) {
                DebugAssert(prefix.find('%') == std::string::npos);
                prefixOut << " " << fmt;
                prefixedFmt  = prefixOut.str();
                uncheckedFmt = prefixedFmt.c_str();
            }
        }

        unique_lock<mutex> lock(sLogMutex);

        if ( _observers ) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
            _observers->notify(
                    RawLogEntry{
                            .timestamp = timestamp,
                            .domain    = *this,
                            .level     = level,
                            .objRef    = objRef,
                            .prefix    = prefix,
                    },
                    uncheckedFmt, args);
#pragma GCC diagnostic pop
        }
        //TODO: honor `doCallback`
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

#pragma mark - OBJECT REFS:

    namespace loginternal {
        static ObjectMap sObjectMap;

        // Must be called from a method holding sLogMutex
        string getObject(LogObjectRef ref) {
            const auto found = sObjectMap.find(ref);
            if ( found != sObjectMap.end() ) { return found->second.first; }

            return "?";
        }

        size_t addObjectPath(char* destBuf, size_t bufSize, LogObjectRef obj) {
            auto objPath = getObjectPath(obj, sObjectMap);
            return snprintf(destBuf, bufSize, "Obj=%s ", objPath.c_str());
        }

        static void getObjectPathRecur(const ObjectMap& objMap, ObjectMap::const_iterator iter, std::stringstream& ss) {
            // pre-conditions: iter != objMap.end()
            if ( iter->second.second != LogObjectRef::None ) {
                auto parentIter = objMap.find(iter->second.second);
                if ( parentIter == objMap.end() ) {
                    // the parent object is deleted. We omit the loggingClassName
                    ss << "/#" << unsigned(iter->second.second);
                } else {
                    getObjectPathRecur(objMap, parentIter, ss);
                }
            }
            ss << "/" << iter->second.first << "#" << unsigned(iter->first);
        }

        std::string getObjectPath(LogObjectRef obj, const ObjectMap& objMap) {
            auto iter = objMap.find(obj);
            if ( iter == objMap.end() ) { return ""; }
            std::stringstream ss;
            getObjectPathRecur(objMap, iter, ss);
            return ss.str() + "/";
        }

        std::string getObjectPath(LogObjectRef obj) { return getObjectPath(obj, sObjectMap); }

        LogObjectRef registerObject(const void* object, const LogObjectRef* val, const string& description,
                                    const string& nickname, LogDomain& domain, LogLevel level) {
            static unsigned sLastObjRef = 0;

            unique_lock<mutex> lock(sLogMutex);
            if ( *val != LogObjectRef::None ) { return *val; }

            LogObjectRef objRef{++sLastObjRef};
            sObjectMap.emplace(std::piecewise_construct, std::forward_as_tuple(objRef),
                               std::forward_as_tuple(nickname, LogObjectRef::None));
            //TODO: Fix this
            /*
            if ( LogCallback::_willLog(level) )
                LogCallback::_invokeCallback(domain, level, "{%s#%u}==> %s @%p", nickname.c_str(), objRef,
                                             description.c_str(), object);
                                             */
            return objRef;
        }

        void unregisterObject(LogObjectRef objectRef) {
            unique_lock<mutex> lock(sLogMutex);
            sObjectMap.erase(objectRef);
        }

        bool registerParentObject(LogObjectRef object, LogObjectRef parentObject) {
            enum { kNoWarning, kNotRegistered, kParentNotRegistered, kAlreadyRegistered } warningCode{kNoWarning};

            {
                unique_lock<mutex> lock(sLogMutex);
                auto               iter = sObjectMap.find(object);
                if ( iter == sObjectMap.end() ) {
                    warningCode = kNotRegistered;
                } else if ( !sObjectMap.contains(parentObject) ) {
                    warningCode = kParentNotRegistered;
                } else if ( iter->second.second != LogObjectRef::None ) {
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
    }  // namespace loginternal

#pragma mark - LOGGING CLASS:

    Logging::~Logging() {
        if ( _objectRef != LogObjectRef::None ) loginternal::unregisterObject(_objectRef);
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
            _objectRef        = registerObject(this, &_objectRef, identifier, nickname, _domain, level);
        }
        return _objectRef;
    }

    void Logging::setParentObjectRef(LogObjectRef parentObjRef) {
        Assert(registerParentObject(getObjectRef(), parentObjRef));
    }

    void Logging::_logv(LogLevel level, const char* format, va_list args) const {
        _domain.computeLevel();
        if ( _domain.willLog(level) ) _domain.vlog(level, this, true, format, args);
    }
}  // namespace litecore