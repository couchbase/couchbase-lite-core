//
// Error_android.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#ifdef __ANDROID__

#include <unwind.h>
#include <dlfcn.h>
#include <android/log.h>
#include <cxxabi.h>
#include "Error.hh"
#include <sstream>

using namespace std;

namespace litecore {

    struct BacktraceState {
        void **current;
        void **end;
    };

    static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context *context, void *arg) {
        BacktraceState *state = static_cast<BacktraceState *>(arg);
        uintptr_t pc = _Unwind_GetIP(context);
        if (pc) {
            if (state->current == state->end) {
                return _URC_END_OF_STACK;
            } else {
                *state->current++ = reinterpret_cast<void *>(pc);
            }
        }
        return _URC_NO_REASON;
    }

    static size_t captureBacktrace(void **buffer, size_t max) {
        BacktraceState state = {buffer, buffer + max};
        _Unwind_Backtrace(unwindCallback, &state);

        return state.current - buffer;
    }

    /*static*/ string error::backtrace(unsigned skip) {
        stringstream out;

        void *buffer[50];
        size_t captured = captureBacktrace(buffer, 50);
        char *unmangled = nullptr;
        size_t unmangledLen = 0;

        for (size_t i = skip + 1; i < captured; i++) {
            const void *addr = buffer[i];
            const char *symbol = "";
            const char *file = "???";

            Dl_info info;
            if (dladdr(addr, &info) && info.dli_sname) {
                int status;
                symbol = info.dli_sname;
                file = info.dli_fname;
                unmangled = abi::__cxa_demangle(info.dli_sname, unmangled, &unmangledLen, &status);
                if (unmangled && status == 0) {
                    symbol = unmangled;
                }
            }

            char *cstr = nullptr;
            asprintf(&cstr, "%s %s", file, symbol);
            out << cstr;
            free(cstr);
        }
        return out.str();
    }
}

#endif
