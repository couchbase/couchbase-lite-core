//
//  LogInternal.h
//  CBForest
//
//  Created by Jens Alfke on 10/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.


#ifndef CBForest_LogInternal_h
#define CBForest_LogInternal_h

#include "Database.hh"

namespace cbforest {

void _Log(logLevel, const char *message, ...);

#ifdef _MSC_VER
    // Apparently vararg macro syntax is slightly different in MSVC than in Clang/GCC
    #define LogAt(LEVEL, MESSAGE, ...) \
                if (LogLevel <= LEVEL) _Log(LEVEL, MESSAGE, __VA_ARGS__);
    #define Debug(MESSAGE, ...)      LogAt(kDebug,   MESSAGE, __VA_ARGS__)
    #define Log(MESSAGE, ...)        LogAt(kInfo,    MESSAGE, __VA_ARGS__)
    #define Warn(MESSAGE, ...)       LogAt(kWarning, MESSAGE, __VA_ARGS__)
    #define WarnError(MESSAGE, ...)  LogAt(kError,   MESSAGE, __VA_ARGS__)
#else
    #define LogAt(LEVEL, MESSAGE...) \
                ({if (__builtin_expect(LogLevel <= LEVEL, false)) _Log(LEVEL, MESSAGE);})
    #define Debug(MESSAGE...)      LogAt(kDebug,   MESSAGE)
    #define Log(MESSAGE...)        LogAt(kInfo,    MESSAGE)
    #define Warn(MESSAGE...)       LogAt(kWarning, MESSAGE)
    #define WarnError(MESSAGE...)  LogAt(kError,   MESSAGE)
#endif

// Debug(...) is stripped out of release builds
#if !DEBUG
    #undef Debug
#ifdef _MSC_VER
#define Debug(MESSAGE, ...)
#else
    #define Debug(MESSAGE...)      ({ })
#endif
#endif

}

#endif
