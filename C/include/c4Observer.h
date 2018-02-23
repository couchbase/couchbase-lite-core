//
// c4Observer.h
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

#include "c4Database.h"

#ifdef __cplusplus
extern "C" {
#endif


    /** \defgroup Observer  Database and Document Observers
        @{ */

    typedef struct {
        C4String docID;
        C4String revID;
        C4SequenceNumber sequence;
        uint32_t bodySize;
    } C4DatabaseChange;

    /** A database-observer reference. */
    typedef struct c4DatabaseObserver C4DatabaseObserver;

    /** Callback invoked by a database observer.
        @param observer  The observer that initiated the callback.
        @param context  user-defined parameter given when registering the callback. */
    typedef void (*C4DatabaseObserverCallback)(C4DatabaseObserver* observer C4NONNULL,
                                               void *context);

    /** Creates a new database observer, with a callback that will be invoked after the database
        changes. The callback will be called _once_, after the first change. After that it won't
        be called again until all of the changes have been read by calling `c4dbobs_getChanges`.
        @param database  The database to observer.
        @param callback  The function to call after the database changes.
        @param context  An arbitrary value that will be passed to the callback.
        @return  The new observer reference. */
    C4DatabaseObserver* c4dbobs_create(C4Database* database C4NONNULL,
                                       C4DatabaseObserverCallback callback C4NONNULL,
                                       void *context) C4API;

    /** Identifies which documents have changed since the last time this function was called, or
        since the observer was created. This function effectively "reads" changes from a stream,
        in whatever quantity the caller desires. Once all of the changes have been read, the
        observer is reset and ready to notify again.

        IMPORTANT: After calling this function, you must call `c4dbobs_releaseChanges` to release
        memory that's being referenced by the `C4DatabaseChange`s.

        @param observer  The observer.
        @param outChanges  A caller-provided buffer of structs into which changes will be
                            written.
        @param maxChanges  The maximum number of changes to return, i.e. the size of the caller's
                            outChanges buffer.
        @param outExternal  Will be set to true if the changes were made by a different C4Database.
        @return  The number of changes written to `outChanges`. If this is less than `maxChanges`,
                            the end has been reached and the observer is reset. */
    uint32_t c4dbobs_getChanges(C4DatabaseObserver *observer C4NONNULL,
                                C4DatabaseChange outChanges[] C4NONNULL,
                                uint32_t maxChanges,
                                bool *outExternal C4NONNULL) C4API;

    /** Releases the memory used by the C4DatabaseChange structs (to hold the docID and revID
        strings.) This must be called after `c4dbobs_getChanges().
        @param changes  The same array of changes that was passed to `c4dbobs_getChanges`.
        @param numChanges  The number of changes returned by `c4dbobs_getChanges`, i.e. the number
                            of valid items in `changes`. */
    void c4dbobs_releaseChanges(C4DatabaseChange changes[],
                                uint32_t numChanges) C4API;

    /** Stops an observer and frees the resources it's using.
        It is safe to pass NULL to this call. */
    void c4dbobs_free(C4DatabaseObserver*) C4API;


    /** A document-observer reference. */
    typedef struct c4DocumentObserver C4DocumentObserver;

    /** Callback invoked by a document observer.
        @param observer  The observer that initiated the callback.
        @param docID  The ID of the document that changed.
        @param sequence  The sequence number of the change.
        @param context  user-defined parameter given when registering the callback. */
    typedef void (*C4DocumentObserverCallback)(C4DocumentObserver* observer C4NONNULL,
                                               C4String docID,
                                               C4SequenceNumber sequence,
                                               void *context);

    /** Creates a new document observer, with a callback that will be invoked when the document
        changes. The callback will be called every time the document changes.
        @param database  The database to observer.
        @param docID  The ID of the document to observe.
        @param callback  The function to call after the database changes.
        @param context  An arbitrary value that will be passed to the callback.
        @return  The new observer reference. */
    C4DocumentObserver* c4docobs_create(C4Database* database C4NONNULL,
                                        C4String docID,
                                        C4DocumentObserverCallback callback,
                                        void *context) C4API;

    /** Stops an observer and frees the resources it's using.
        It is safe to pass NULL to this call. */
    void c4docobs_free(C4DocumentObserver*) C4API;

    /** @} */
#ifdef __cplusplus
}
#endif
