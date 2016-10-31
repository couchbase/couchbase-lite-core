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
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <sstream>
#if __APPLE__
#include <sys/time.h>
#endif
#if __ANDROID__
#include <android/log.h>
#endif
#ifdef _MSC_VER
#include "asprintf.h"
#endif


namespace litecore {

    static void defaultCallback(const LogDomain &domain, LogLevel, const char *message);

    LogLevel LogDomain::MinLevel = LogLevel::Info;
    LogDomain* LogDomain::sFirstDomain = nullptr;
    void (*LogDomain::Callback)(const LogDomain&, LogLevel, const char *message) = defaultCallback;

    LogDomain DefaultLog("", LogLevel::Info);


    void LogDomain::log(LogLevel level, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vlog(level, fmt, args);
        va_end(args);
    }


    void LogDomain::vlog(LogLevel level, const char *fmt, va_list args) {
        if (_level == LogLevel::Uninitialized) {
            char *val = getenv((std::string("LiteCoreLog") + _name).c_str());
            if (val)
                _level = LogLevel::Info;
            else
                _level = LogLevel::Warning;
        }
        if (!willLog(level) || level < MinLevel || !Callback)
            return;
        char *message;
        if (vasprintf(&message, fmt, args) < 0) {
            Callback(*this, level, "(Failed to allocate memory for log message)");
            return;
        }
        Callback(*this, level, message);
        free(message);
    }


    static void defaultCallback(const LogDomain &domain, LogLevel level, const char *message) {
        #ifdef __ANDROID__
            static const int kLevels[5] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_INFO,
                                           ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
            static char source[100] = "LiteCore";
            auto name = domain.name();
            if (name[0]) {
                strcat(source, " ");
                strcat(source, name);
            }
            __android_log_write(kLevels[level], source, message);
        #else
            #if __APPLE__
                struct timeval tv;
                gettimeofday(&tv, nullptr);
                time_t now = tv.tv_sec;
            #else
                time_t now = time(nullptr);
            #endif
            struct tm tm;
            localtime_r(&now, &tm);
            char timestamp[100];
            strftime(timestamp, sizeof(timestamp), "%T", &tm);
            #if __APPLE__
                sprintf(timestamp + strlen(timestamp), ".%03d", tv.tv_usec / 1000);
            #endif

            auto name = domain.name();
            static const char *kLevels[] = {"***", "", "", "WARNING:", "ERROR: "};
            fprintf(stderr, "%s| %s%s %s%s\n",
                    timestamp, name, (name[0] ? ":" :""), kLevels[(int)level], message);
        #endif
    }


    std::string _logSlice(fleece::slice s) {
        std::stringstream o;
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
            o << "\"" << std::string((char*)s.buf, s.size) << "\"";
        }
        return o.str();
    }
    
    

}
