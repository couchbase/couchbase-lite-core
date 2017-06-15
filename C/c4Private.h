//
//  c4Private.h
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 1/21/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "c4Document.h"
#include "c4Replicator.h"

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {

    CBL_CORE_API extern std::atomic_int gC4InstanceCount;
    CBL_CORE_API extern std::atomic_int gC4ExpectExceptions;

#else
    CBL_CORE_API extern atomic_int gC4InstanceCount;
    CBL_CORE_API extern atomic_int gC4ExpectExceptions;
#endif

void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error *outError) C4API;

void c4log_warnOnErrors(bool) C4API;

void c4db_lock(C4Database *db) C4API;
void c4db_unlock(C4Database *db) C4API;

C4Document* c4doc_getForPut(C4Database *database,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError) C4API;

C4Socket* c4socket_fromNative(C4SocketFactory factory,
                              void *nativeHandle,
                              const C4Address *address) C4API;

C4Replicator* c4repl_newWithSocket(C4Database* db,
                                   C4Socket *openSocket,
                                   C4ReplicatorMode push,
                                   C4ReplicatorMode pull,
                                   C4Slice optionsDictFleece,
                                   C4ReplicatorStatusChangedCallback onStatusChanged,
                                   void *callbackContext,
                                   C4Error *outError) C4API;

#ifdef __cplusplus
}




/** Base class that keeps track of the total instance count of it and all subclasses.
    This is useful for leak detection. */
class C4InstanceCounted {
public:
    C4InstanceCounted()                             {++gC4InstanceCount;}
    C4InstanceCounted(const C4InstanceCounted&)     {++gC4InstanceCount;}
    ~C4InstanceCounted()                            {--gC4InstanceCount;}
};

#endif
