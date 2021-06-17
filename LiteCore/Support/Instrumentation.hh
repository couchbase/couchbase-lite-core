//
//  Instrumentation.hh
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

#pragma once
#include <stdint.h>

namespace litecore {

#ifdef __APPLE__
#define LITECORE_SIGNPOSTS 1
#endif

    /** A utility for logging chronological points and regions of interest, for profiling. */
    class Signpost {
    public:
        enum Type {
            transaction = 1,            // begin/end
            replicatorConnect,
            replicatorDisconnect,
            replication,                // begin/end
            changesBackPressure, // 5
            revsBackPressure,
            handlingChanges,
            handlingRev,
            blipReceived,
            blipSent,           // 10
        };

#if LITECORE_SIGNPOSTS
        static void mark(Type, uintptr_t param =0, uintptr_t param2 =0);
        static void begin(Type, uintptr_t param =0, uintptr_t param2 =0);
        static void end(Type, uintptr_t param =0, uintptr_t param2 =0);

        Signpost(Type t, uintptr_t param1 =0, uintptr_t param2 =0)
        :_type(t), _param1(param1), _param2(param2)
        {begin(_type, _param1, _param2);}
        ~Signpost()
        {end(_type, _param1, _param2);}

    private:
        Type const _type;
        uintptr_t _param1, _param2;

#else
        static inline void mark(Type, uintptr_t param =0, uintptr_t param2 =0)    { }
        static inline void begin(Type, uintptr_t param =0, uintptr_t param2 =0)   { }
        static inline void end(Type, uintptr_t param =0, uintptr_t param2 =0)     { }

        Signpost(Type t)                                    { }
        ~Signpost()                                         =default;
#endif
    };

}
