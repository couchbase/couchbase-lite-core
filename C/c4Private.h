//
// c4Private.h
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
#include "c4Document.h"
#include "c4Replicator.h"

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {

    /** Total number of C4 objects whose classes inherit from C4InstanceCounted (see below).
        This is compared by unit tests before and after the test runs, to check for leaks. */
    CBL_CORE_API extern std::atomic_int gC4InstanceCount;

    /** If > 0, the currently running test is expected to throw an exception, so debuggers should
        ignore the exception. */
    CBL_CORE_API extern std::atomic_int gC4ExpectExceptions;

#else
    CBL_CORE_API extern atomic_int gC4InstanceCount;
    CBL_CORE_API extern atomic_int gC4ExpectExceptions;
#endif

/** Stores a C4Error in `*outError`. */
void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error *outError) C4API;

/** If set to true, LiteCore will log a warning of the form "LiteCore throwing %s error %d: %s"
    just before throwing an internal exception. This can be a good way to catch the source where
    an error occurs. */
void c4log_warnOnErrors(bool) C4API;

/** Locks a recursive mutex associated with the C4Database instance.
    Blocks if it's already locked. */
void c4db_lock(C4Database *db) C4API;

/** Unlocks the mutex locked by c4db_lock. */
void c4db_unlock(C4Database *db) C4API;

/** Compiles a JSON query and returns the result set as JSON: an array with one item per result,
    and each result is an array of columns. */
C4SliceResult c4db_rawQuery(C4Database *database C4NONNULL, C4String query, C4Error *outError) C4API;

/** Subroutine of c4doc_put that reads the current revision of the document.
    Only exposed for testing; see the unit test "Document GetForPut". */
C4Document* c4doc_getForPut(C4Database *database C4NONNULL,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError) C4API;

/** Converts C4DocumentFlags to the equivalent C4RevisionFlags. */
C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags);

/** Sets the document flag kSynced. Used by the replicator to track synced documents. */
bool c4db_markSynced(C4Database *database,
                     C4String docID,
                     C4SequenceNumber sequence,
                     C4RemoteID remoteID,
                     C4Error *outError) C4API;

/** Constructs a C4Socket from a "native handle", whose interpretation is up to the registered
    C4SocketFactory. */
C4Socket* c4socket_fromNative(C4SocketFactory factory,
                              void *nativeHandle C4NONNULL,
                              const C4Address *address C4NONNULL) C4API;

/** Creates a replicator from an already-open C4Socket. Called by the P2P listener. */
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
