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
        C4String scheme;
        C4String hostname;
        uint16_t port;
        C4String path;
    } C4Address;

    /** How to replicate, in either direction */
    typedef enum {
        kC4Disabled,        // Do not allow this direction
        kC4Passive,         // Allow peer to initiate this direction
        kC4OneShot,         // Replicate, then stop
        kC4Continuous       // Keep replication active until stopped by application
    } C4ReplicatorMode;

    typedef enum {
        kC4Stopped,
        kC4Connecting,
        kC4Idle,
        kC4Busy
    } C4ReplicatorActivityLevel;

    /** Current status of replication. Passed to callback. */
    typedef struct {
        C4ReplicatorActivityLevel level;
        C4Error error;
    } C4ReplicatorState;


    /** Opaque reference to a replicator. */
    typedef struct C4Replicator C4Replicator;

    /** Callback a client can register, to get progress information. */
    typedef void (*C4ReplicatorStateChangedCallback)(C4Replicator*,
                                                     C4ReplicatorState,
                                                     void *context);

    /** Creates a new replicator. */
    C4Replicator* c4repl_new(C4Database* db,
                             C4Address address,
                             C4ReplicatorMode push,
                             C4ReplicatorMode pull,
                             C4ReplicatorStateChangedCallback onStateChanged,
                             void *callbackContext,
                             C4Error *err) C4API;

    /** Frees a replicator reference. If the replicator is running it will stop. */
    void c4repl_free(C4Replicator* repl) C4API;

    /** Tells a replicator to stop. */
    void c4repl_stop(C4Replicator* repl) C4API;

    /** Returns the current state of a replicator. */
    C4ReplicatorState c4repl_getState(C4Replicator *repl) C4API;


    /** @} */

#ifdef __cplusplus
}
#endif
