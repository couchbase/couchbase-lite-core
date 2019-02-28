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
            get,
            replication,                // begin/end
            replicatorConnect,
            replicatorDisconnect,
        };

#if LITECORE_SIGNPOSTS
        static void mark(Type, uint32_t param =0);
        static void begin(Type, uint32_t param =0);
        static void end(Type, uint32_t param =0);

        Signpost(Type t)            :_type(t) {begin(_type);}
        ~Signpost()                 {end(_type);}

    private:
        Type const _type;

#else
        static inline void mark(Type, uint32_t param =0)    { }
        static inline void begin(Type, uint32_t param =0)   { }
        static inline void end(Type, uint32_t param =0)     { }

        Signpost(Type t)                                    { }
        ~Signpost()                                         { }
#endif
    };

}
