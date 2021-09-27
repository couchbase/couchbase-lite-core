//
//  Instrumentation.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
