//
// Logging_Stub.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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
