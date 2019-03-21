//
//  Instrumentation.cc
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

#include "Instrumentation.hh"

#ifdef __APPLE__
#include <sys/kdebug_signpost.h>
#endif

namespace litecore {

#if defined(__APPLE__) && LITECORE_SIGNPOSTS
    enum Color {
        blue, green, purple, orange, red    // used for last argument
    };

    void Signpost::mark(Type t, uintptr_t param, uintptr_t param2) {
        if (__builtin_available(macOS 10.12, iOS 10, tvOS 10, *))
            kdebug_signpost(t, param, param2, 0, (t % 5));
    }

    void Signpost::begin(Type t, uintptr_t param, uintptr_t param2) {
        if (__builtin_available(macOS 10.12, iOS 10, tvOS 10, *))
            kdebug_signpost_start(t, param, param2, 0, (t % 5));
    }

    void Signpost::end(Type t, uintptr_t param, uintptr_t param2) {
        if (__builtin_available(macOS 10.12, iOS 10, tvOS 10, *))
            kdebug_signpost_end(t, param, param2, 0, (t % 5));
    }
#endif

}
