//
//  c4Replicator.h
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4Socket.h"
#include "c4Database.h"

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Replicator Replicator
        @{ */

#define kC4Replicator2Scheme    C4STR("blip")
#define kC4Replicator2TLSScheme C4STR("blips")

    /** How to replicate, in either direction */
    typedef C4_ENUM(int32_t, C4ReplicatorMode) {
        kC4Disabled,        // Do not allow this direction
        kC4Passive,         // Allow peer to initiate this direction
        kC4OneShot,         // Replicate, then stop
        kC4Continuous       // Keep replication active until stopped by application
    };

    typedef C4_ENUM(int32_t, C4ReplicatorActivityLevel) {
        kC4Stopped,
        kC4Offline,
        kC4Connecting,
        kC4Idle,
        kC4Busy
    };

    extern const char* const kC4ReplicatorActivityLevelNames[5];


    typedef struct {
        uint64_t    completed;
        uint64_t    total;
    } C4Progress;

    /** Current status of replication. Passed to callback. */
    typedef struct {
        C4ReplicatorActivityLevel level;
        C4Progress progress;
        C4Error error;
    } C4ReplicatorStatus;


    /** Opaque reference to a replicator. */
    typedef struct C4Replicator C4Replicator;

    /** Callback a client can register, to get progress information.
        This will be called on arbitrary background threads, and should not block. */
    typedef void (*C4ReplicatorStatusChangedCallback)(C4Replicator*,
                                                      C4ReplicatorStatus,
                                                      void *context);

    /** Creates a new replicator. */
    C4Replicator* c4repl_new(C4Database* db,
                             C4Address remoteAddress,
                             C4String remoteDatabaseName,
                             C4Database* otherLocalDB,
                             C4ReplicatorMode push,
                             C4ReplicatorMode pull,
                             C4ReplicatorStatusChangedCallback onStateChanged,
                             void *callbackContext,
                             C4Error *err) C4API;

    /** Frees a replicator reference. If the replicator is running it will stop. */
    void c4repl_free(C4Replicator* repl) C4API;

    /** Tells a replicator to stop. */
    void c4repl_stop(C4Replicator* repl) C4API;

    /** Returns the current state of a replicator. */
    C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl) C4API;


    /** @} */

#ifdef __cplusplus
}
#endif
