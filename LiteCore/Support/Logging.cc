//
//  Logging.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/16/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Logging.hh"
#include "StringUtil.hh"
#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include "PlatformIO.hh"

#if __APPLE__
#include <sys/time.h>
#endif
#if __ANDROID__
#include <android/log.h>
#endif

#if defined(__clang__) && !defined(__ANDROID__)
#include <cxxabi.h>
#endif

using namespace std;


namespace litecore {

    static void defaultCallback(const LogDomain&, LogLevel, const char *message, va_list);

    LogLevel LogDomain::MinLevel = LogLevel::Info;
    LogDomain* LogDomain::sFirstDomain = nullptr;
    LogDomain::Callback_t LogDomain::Callback = defaultCallback;

    static ofstream *sEncodedOut = nullptr;
    static LogEncoder* sLogEncoder = nullptr;
    static mutex sLogMutex;


    void LogDomain::writeEncodedLogsTo(const string &filePath) {
        unique_lock<mutex> lock(sLogMutex);
        delete sLogEncoder;
        sLogEncoder = nullptr;
        delete sEncodedOut;
        sEncodedOut = nullptr;
        if (!filePath.empty()) {
            sEncodedOut = new ofstream(filePath, ofstream::out | ofstream::trunc | ofstream::binary);
            sLogEncoder = new LogEncoder(*sEncodedOut);

            // Make sure to flush the log when the process exits:
            static once_flag f;
            call_once(f, []{
                atexit([]{
                    unique_lock<mutex> lock(sLogMutex);
                    if (sLogEncoder)
                        sLogEncoder->log((int)LogLevel::Info, "", LogEncoder::None,
                                         "---- END ----");
                    delete sLogEncoder;
                    delete sEncodedOut;
                    sLogEncoder = nullptr;
                    sEncodedOut = nullptr;
                });
            });
        }
    }


    void LogDomain::log(LogLevel level, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, fmt, args);
        va_end(args);
    }


    LogLevel LogDomain::initLevel() {
        if (_level == LogLevel::Uninitialized) {
            char *val = getenv((string("LiteCoreLog") + _name).c_str());
            _level = LogLevel::Info;
            if (val) {
                static const char* const kLevelNames[] = {"debug", "verbose", "info",
                                                          "warning", "error", "none", nullptr};
                for (int i = 0; kLevelNames[i]; i++)
                    if (0 == strcasecmp(val, kLevelNames[i])) {
                        _level = LogLevel(i);
                        break;
                    }
            }
            if (_level > MinLevel)
                _level = MinLevel;
        }
        return _level;
    }


    static char sFormatBuffer[2048];


    void LogDomain::vlog(LogLevel level, unsigned objRef, const char *fmt, va_list args) {
        if (_level == LogLevel::Uninitialized)
            initLevel();
        if (!willLog(level))
            return;

        unique_lock<mutex> lock(sLogMutex);
        if (Callback) {
            va_list args2;
            va_copy(args2, args);
            if (objRef) {
                snprintf(sFormatBuffer, sizeof(sFormatBuffer), "{%u} %s", objRef, fmt);
                Callback(*this, level, sFormatBuffer, args2);
            } else {
                Callback(*this, level, fmt, args2);
            }
            va_end(args2);
        }

        if (sLogEncoder) {
            sLogEncoder->vlog((int8_t)level, _name, (LogEncoder::ObjectRef)objRef, fmt, args);
        }
    }


    void LogDomain::vlog(LogLevel level, const char *fmt, va_list args) {
        vlog(level, LogEncoder::None, fmt, args);
    }


    static void invokeCallback(LogDomain &domain, LogLevel level, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        LogDomain::Callback(domain, level, fmt, args);
        va_end(args);
    }


    unsigned LogDomain::registerObject(const std::string &description, LogLevel level) {
        unique_lock<mutex> lock(sLogMutex);
        unsigned objRef;
        if (sLogEncoder)
            objRef = sLogEncoder->registerObject(description);
        else
            objRef = ++_lastObjRef;

        if (Callback)
            invokeCallback(*this, level, "{%u}--> %s", objRef, description.c_str());
        return objRef;
    }


    static void defaultCallback(const LogDomain &domain, LogLevel level,
                                const char *fmt, va_list args)
    {
        #ifdef __ANDROID__
            static const int kLevels[5] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_INFO,
                                           ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
            static char source[100] = "LiteCore";
            auto name = domain.name();
            if (name[0]) {
                strcat(source, " ");
                strcat(source, name);
            }
            vsnprintf(sFormatBuffer, sizeof(sFormatBuffer), fmt, args);
            __android_log_write(kLevels[(int)level], source, sFormatBuffer);
        #else
            auto name = domain.name();
            static const char *kLevels[] = {"***", "", "", "WARNING", "ERROR"};
            LogDecoder::writeTimestamp(LogDecoder::now(), cerr);
            LogDecoder::writeHeader(kLevels[(int)level], name, cerr);
            vfprintf(stderr, fmt, args);
            fputc('\n', stderr);
        #endif
    }

#ifndef C4_TESTS
    string _logSlice(fleece::slice s) {
        stringstream o;
        if (s.buf == nullptr) {
            return "<null>";
        } else {
            auto buf = (const uint8_t*)s.buf;
            for (size_t i = 0; i < s.size; i++) {
                if (buf[i] < 32 || buf[i] > 126) {
                    o << "<" << s.hexString() << ">";
                    return o.str();
                }
            }
            o << "\"" << string((char*)s.buf, s.size) << "\"";
        }
        return o.str();
    }
#endif

    LogDomain* LogDomain::named(const char *name) {
        if (!name)
            name = "";
        for (auto d = sFirstDomain; d; d = d->_next)
            if (strcmp(d->name(), name) == 0)
                return d;
        return nullptr;
    }


#pragma mark - LOGGING CLASS:


    std::string Logging::loggingIdentifier() const {
		// Get the name of my class, unmangle it, and remove namespaces:
		const char *name = typeid(*this).name();
#if defined(__clang__) && !defined(__ANDROID__)
        size_t unmangledLen;
        int status;
        char *unmangled = abi::__cxa_demangle(name, nullptr, &unmangledLen, &status);
        if (unmangled) {
            auto colon = strrchr(unmangled, ':');
            name = colon ? colon + 1 : unmangled;
        }

        auto result = format("%s %p", name, this);
        free(unmangled);
        return result;
#else
        return format("%s %p", name, this);
#endif
    }


    void Logging::_log(LogLevel level, const char *format, ...) const {
        va_list args;
        va_start(args, format);
        _logv(level, format, args);
        va_end(args);
    }
    
    void Logging::_logv(LogLevel level, const char *format, va_list args) const {
#if DEBUG
        if (!_domain.willLog(level))
            return;
#endif
        if (!_objectRef) {
            string identifier = loggingIdentifier();
            const_cast<Logging*>(this)->_objectRef = _domain.registerObject(identifier, level);
        }
        _domain.vlog(level, _objectRef, format, args);
    }


}
