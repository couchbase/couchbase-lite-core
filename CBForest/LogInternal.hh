//
//  LogInternal.h
//  CBForest
//
//  Created by Jens Alfke on 10/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef CBForest_LogInternal_h
#define CBForest_LogInternal_h

#include "Database.hh"

namespace forestdb {

void _Log(logLevel, const char *message, ...);

#define Debug(MESSAGE...)      if(LogLevel > kDebug) ; else _Log(kDebug, MESSAGE)
#define Log(MESSAGE...)        if(LogLevel > kInfo) ; else _Log(kInfo, MESSAGE)
#define Warn(MESSAGE...)       if(LogLevel > kWarning) ; else _Log(kWarning, MESSAGE)
#define WarnError(MESSAGE...)  if(LogLevel > kError) ; else _Log(kError, MESSAGE)

}

#endif
