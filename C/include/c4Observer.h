//
// c4Observer.h
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "c4DocumentTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup Observer  Collection (Database), Document, Query Observers
        @{ */

/** \name Collection Observer
        @{ */

/** Callback invoked by a collection/database observer.
     
        CAUTION: This callback is called when a transaction is committed, even one made by a
        different connection (C4Database instance) on the same file. This means that, if your
        application is multithreaded, the callback may be running on a different thread than the
        one this database instance uses. It is your responsibility to ensure thread safety.

        In general, it is best to make _no_ LiteCore calls from within this callback. Instead,
        use your platform event-handling API to schedule a later call from which you can read the
        changes. Since this callback may be invoked many times in succession, make sure you
        schedule only one call at a time.

        @param observer  The observer that initiated the callback.
        @param context  user-defined parameter given when registering the callback. */
typedef void (*C4CollectionObserverCallback)(C4CollectionObserver* observer, void* C4NULLABLE context);


/** Creates a new collection observer, with a callback that will be invoked after one or more
        documents in the collection have changed.
        This is exactly like \ref c4dbobs_create, except that it acts on any collection.
        @param collection  The collection to observe.
        @param callback  The function to call after the collection changes.
        @param context  An arbitrary value that will be passed to the callback.
        @return  The new observer reference. */
NODISCARD CBL_CORE_API C4CollectionObserver* c4dbobs_createOnCollection(C4Collection*                collection,
                                                                        C4CollectionObserverCallback callback,
                                                                        void* C4NULLABLE             context,
                                                                        C4Error* C4NULLABLE          error) C4API;

/** Identifies which documents have changed in the collection since the last time this function
        was called, or since the observer was created. This function effectively "reads" changes
        from a stream, in whatever quantity the caller desires. Once all of the changes have been
        read, the observer is reset and ready to notify again.

        This function is usually called in response to your `C4CollectionObserverCallback` being
        called, but it doesn't have to be; it can be called at any time (subject to thread-safety
        requirements, of course.)

        \warning After calling this function, you must call \ref c4dbobs_releaseChanges to release
        memory that's being referenced by the `C4CollectionChange`s.

        @param observer  The observer.
        @param outChanges  A caller-provided buffer of structs into which changes will be
                            written.
        @param maxChanges  The maximum number of changes to return, i.e. the size of the caller's
                            outChanges buffer.
        @return  Common information about the changes contained in outChanges (number of changes, 
                 external vs non-external, and the relevant collection) */
NODISCARD CBL_CORE_API C4CollectionObservation c4dbobs_getChanges(C4CollectionObserver* observer,
                                                                  C4CollectionChange    outChanges[C4NONNULL],
                                                                  uint32_t              maxChanges) C4API;

/** Releases the memory used by the `C4CollectionChange` structs (to hold the docID and revID
        strings.) This must be called after \ref c4dbobs_getChanges().
        @param changes  The same array of changes that was passed to \ref c4dbobs_getChanges.
        @param numChanges  The number of changes returned by \ref c4dbobs_getChanges, i.e. the number
                            of valid items in `changes`. */
CBL_CORE_API void c4dbobs_releaseChanges(C4CollectionChange changes[C4NONNULL], uint32_t numChanges) C4API;

/** @} */


/** \name Document Observer
     @{ */

/** Callback invoked by a document observer.
        @param observer  The observer that initiated the callback.
        @param docID  The ID of the document that changed.
        @param sequence  The sequence number of the change.
        @param context  user-defined parameter given when registering the callback. */
typedef void (*C4DocumentObserverCallback)(C4DocumentObserver* observer, C4Collection* collection, C4String docID,
                                           C4SequenceNumber sequence, void* C4NULLABLE context);


/** Creates a new document observer, with a callback that will be invoked when the document
        changes.
        \note This is exactly like \ref c4docobs_create, except that it works on any collection.
        @param collection  The collection containing the document to observe.
        @param docID  The ID of the document to observe.
        @param callback  The function to call after the database changes.
        @param context  An arbitrary value that will be passed to the callback.
        @return  The new observer reference. */
NODISCARD CBL_CORE_API C4DocumentObserver* c4docobs_createWithCollection(C4Collection* collection, C4String docID,
                                                                         C4DocumentObserverCallback callback,
                                                                         void* C4NULLABLE           context,
                                                                         C4Error* C4NULLABLE        error) C4API;

/** @} */


/** \name Query Observer
        @{
        A query observer, also called a "live query", notifies the client when the query's result
        set changes. (Not just any time the database changes.)

        This is done as follows, starting from when the first time an observer on a particular
        query is enabled:

        1. A separate C4Query instance is created, on a separate database instance
           (there's one of these background database instances per C4Database.)
        2. The copied query is run on a background thread, and it saves its results.
        3. The query observer(s) are notified so they can see the initial results.
        4. The background thread listens for changes to the database, _or_ changes to the query
           parameters (\ref c4query_setParameters). In response:
           - If it's been less than 250ms since the last time it ran the query, it first waits
             500ms; during this time it ignores further database changes.
           - It runs the query.
           - It compares the new result set to the old one; if they're different, it saves the
             new results and notifies observers. Otherwise it does nothing.
        6. This background task stops when the last observer is disabled.

        Some notes on performance:
     
        * All C4Queries on a single C4Database share a single background C4Database, which can
          only do one thing at a time. That means multiple live queries can bog down since they
          have to run one after the other.
        * The first time any query observer is added in a given _C4Database_, the background
          database instance has to be opened, which takes a few milliseconds.
        * The first time an observer is added to a C4Query, a copy of that query has to be
          created and compiled by the background database, which can also take a few millseconds.
        * Running a C4Query before adding an observer is a bit of a waste, because the query will
          be run twice. It's more efficient to skip running it, and instead wait for the first
          call to the observer.
        * The timing logic in step 4 is a heuristic to provide low latency on occasional database
          changes, but prevent rapid database changes (as happen during pull replication) from
          running the query constantly and/or spamming observers with notifications.
          (The specific times are not currently alterable; they're constants in LiveQuerier.cc.)
     */

/** Callback invoked by a query observer, notifying that the query results have changed.
        The actual enumerator is not passed to the callback, but can be retrieved by calling
        \ref c4queryobs_getEnumerator.
        @warning  This function is called on a random background thread! Be careful of thread
        safety. Do not spend too long in this callback or other observers may be delayed.
        It's best to do nothing except schedule a call on your preferred thread/queue.
        @param observer  The observer triggering the callback.
        @param query  The C4Query that the observer belongs to.
        @param context  The `context` parameter you passed to \ref c4queryobs_create. */
typedef void (*C4QueryObserverCallback)(C4QueryObserver* observer, C4Query* query, void* C4NULLABLE context);

/** Creates a new query observer, with a callback that will be invoked when the query
        results change, with an enumerator containing the new results.
        \note The callback isn't invoked immediately after a change, and won't be invoked after
        every change, to avoid performance problems. Instead, there's a brief delay so multiple
        changes can be coalesced.
        \note The new observer needs to be enabled by calling \ref c4queryobs_setEnabled.*/
NODISCARD CBL_CORE_API C4QueryObserver* c4queryobs_create(C4Query* query, C4QueryObserverCallback callback,
                                                          void* C4NULLABLE context) C4API;

/** Enables a query observer so its callback can be called, or disables it to stop callbacks.

        When a query observer is enabled, its callback will be called with the current results.
        If this is the first observer, the query has to run first (on a background thread) so
        the callback will take a little while; if there are already enabled observers, the
        callback will be pretty much instantaneous.
     */
CBL_CORE_API void c4queryobs_setEnabled(C4QueryObserver* obs, bool enabled) C4API;

/** Returns the current query results, if any.
        When the observer is created, the results are initially NULL until the query finishes
        running in the background.
        Once the observer callback is called, the results are available.
        \note  You are responsible for releasing the returned reference.
        @param obs  The query observer.
        @param forget  If true, the observer will not hold onto the enumerator, and subsequent calls
                    will return NULL until the next time the observer notifies you. This can help
                    conserve memory, since the query result data will be freed as soon as you
                    release the enumerator.
        @param error  If the last evaluation of the query failed, the error will be stored here.
        @return  The current query results, or NULL if the query hasn't run or has failed. */
NODISCARD CBL_CORE_API C4QueryEnumerator* C4NULLABLE c4queryobs_getEnumerator(C4QueryObserver* obs, bool forget,
                                                                              C4Error* C4NULLABLE error) C4API;

/** @} */

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
