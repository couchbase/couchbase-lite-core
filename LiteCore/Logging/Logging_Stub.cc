//
// Logging_Stub.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Logging.hh"
#include "c4Log.h"

/** This source file should be used instead of Logging.cc in libraries that link with the LiteCore
    dynamic library. It simply sends log messages to c4log. Using Logging.cc would create a second
    copy of the logging system with different state, which would cause confusion. */

namespace litecore {

    void LogDomain::log(LogLevel level, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        c4vlog(C4LogDomain(this), C4LogLevel(level), fmt, args);
        va_end(args);
    }

}  // namespace litecore
