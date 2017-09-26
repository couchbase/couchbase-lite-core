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

C4SliceResult cdb_rawQuery(C4Database *database C4NONNULL, C4String query, C4Error *outError) C4API;

C4Document* c4doc_getForPut(C4Database *database C4NONNULL,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError) C4API;

C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags);


bool c4db_markSynced(C4Database *database, C4String docID, C4SequenceNumber sequence);

C4Socket* c4socket_fromNative(C4SocketFactory factory,
                              void *nativeHandle C4NONNULL,
                              const C4Address *address C4NONNULL) C4API;

C4Replicator* c4repl_newWithSocket(C4Database* db C4NONNULL,
                                   C4Socket *openSocket C4NONNULL,
                                   C4ReplicatorParameters params,
                                   C4Error *outError) C4API;

#ifdef __cplusplus
}




/** Base class that keeps track of the total instance count of it and all subclasses.
    This is useful for leak detection. */
class C4InstanceCounted {
public:
#if DEBUG
    C4InstanceCounted()                             {++gC4InstanceCount; track();}
    C4InstanceCounted(const C4InstanceCounted&)     {++gC4InstanceCount; track();}
    virtual ~C4InstanceCounted()                    {--gC4InstanceCount; untrack();}

    void track() const;
    void untrack() const;
#else
    C4InstanceCounted()                             {++gC4InstanceCount;}
    C4InstanceCounted(const C4InstanceCounted&)     {++gC4InstanceCount;}
    ~C4InstanceCounted()                            {--gC4InstanceCount;}
#endif
};

#endif
