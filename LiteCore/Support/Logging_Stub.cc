//
//  Logging_Stub.cc
//  LiteCore
//
//  Created by Jens Alfke on 5/5/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Logging.hh"
#include "c4Base.h"

/** This source file should be used instead of Logging.cc in libraries that link with the LiteCore
    dynamic library. It simply sends log messages to c4log. Using Logging.cc would create a second
    copy of the logging system with different state, which would cause confusion. */

namespace litecore {

    void LogDomain::log(LogLevel level, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        c4vlog(kC4DefaultLog, (C4LogLevel)level, fmt, args);
        va_end(args);
    }

}
