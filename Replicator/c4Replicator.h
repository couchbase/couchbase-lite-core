//
//  c4Replicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Replicator Replicator
        @{ */

    typedef struct {
        C4Slice scheme;
        C4Slice hostname;
        uint16_t port;
        C4Slice path;
    } C4Address;

    typedef struct {
        bool push;
        bool pull;
        bool continuous;
    } C4ReplicateOptions;

    typedef struct _c4Replicator C4Replicator;

    C4Replicator* c4repl_new(C4Database* db,
                             C4Address address,
                             C4ReplicateOptions options,
                             C4Error *err) C4API;

    /** @} */

#ifdef __cplusplus
}
#endif
