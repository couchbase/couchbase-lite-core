//
//  c4Replicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4Database.h"

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Replicator Replicator
        @{ */

    /** A simple parsed-URL type */
    typedef struct {
        C4Slice scheme;
        C4Slice hostname;
        uint16_t port;
        C4Slice path;
    } C4Address;

    /** How to replicate, in either direction */
    typedef enum {
        kC4Disabled,        // Do not allow this direction
        kC4Passive,         // Allow peer to initiate this direction
        kC4OneShot,         // Replicate, then stop
        kC4Continuous       // Keep replication active until stopped by application
    } C4ReplicationMode;

    typedef enum {
        kStopped,
        kConnecting,
        kIdle,
        kBusy
    } C4ReplicationState;

    typedef struct C4Replicator C4Replicator;

    typedef void (*C4ReplicatorStateChangedCallback)(C4Replicator*,
                                                     C4ReplicationState,
                                                     C4Error);

    typedef struct {
        C4ReplicationMode push;
        C4ReplicationMode pull;
        C4ReplicatorStateChangedCallback onStateChanged;
    } C4ReplicationOptions;


    C4Replicator* c4repl_new(C4Database* db,
                             C4Address address,
                             C4ReplicationOptions options,
                             C4Error *err) C4API;

    void c4repl_free(C4Replicator* repl) C4API;

    C4ReplicationState c4repl_getState(C4Replicator *repl) C4API;


    /** @} */

#ifdef __cplusplus
}
#endif
