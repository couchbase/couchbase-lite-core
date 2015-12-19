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
    #define Debug(MESSAGE, ...)      if(LogLevel > kDebug) ; else _Log(kDebug, MESSAGE, __VA_ARGS__)
    #define Log(MESSAGE, ...)        if(LogLevel > kInfo) ; else _Log(kInfo, MESSAGE, __VA_ARGS__)
    #define Warn(MESSAGE, ...)       if(LogLevel > kWarning) ; else _Log(kWarning, MESSAGE, __VA_ARGS__)
    #define WarnError(MESSAGE, ...)  if(LogLevel > kError) ; else _Log(kError, MESSAGE, __VA_ARGS__)
#else
    #define Debug(MESSAGE...)      if(LogLevel > kDebug) ; else _Log(kDebug, MESSAGE)
    #define Log(MESSAGE...)        if(LogLevel > kInfo) ; else _Log(kInfo, MESSAGE)
    #define Warn(MESSAGE...)       if(LogLevel > kWarning) ; else _Log(kWarning, MESSAGE)
    #define WarnError(MESSAGE...)  if(LogLevel > kError) ; else _Log(kError, MESSAGE)
#endif

}

#endif
