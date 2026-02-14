//
// ReplicatorTuning.hh
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include <chrono>
#include <cstdlib>

namespace litecore::repl::tuning {

    using namespace std::chrono;

    //// DBWorker:

    /* Number of new revisions to accumulate in memory before inserting them into the DB.
           (Actually the queue may grow larger than this, since the insertion is triggered
           asynchronously, and more revs may be added to the queue before it happens.) */
    constexpr size_t kInsertionBatchSize = 100;

    /* How long revisions can stay in the queue before triggering insertion into the DB,
           if the queue size hasn't reached kInsertionBatchSize yet. */
    constexpr auto kInsertionDelay = 20ms;

    /* Minimum document body size that will be considered for delta compression.
            (This is the size of the Fleece encoding, which is usually smaller than the JSON.)
           This is not declared `constexpr`, so that the delta-sync unit tests can change it. */
    extern size_t kMinBodySizeForDelta;  // = 200;

    //// Puller:

    /* Number of revisions the peer should include in a single `changes` / `proposeChanges`
            message. (This is sent as a parameter in the puller's opening `subChanges` message.) */
    constexpr unsigned kChangesBatchSize = 200;

    /* The value for the `sendReplacementRevs property on the `subChanges` message we send to the remote during
     * pull replication. If true, when the remote is sending a changes message and a document is updated before
     * the body is sent (which will mean the body for the rev we requested is lost), the remote will send the
     * newest body instead.
     */
    constexpr bool kChangesReplacementRevs = true;

    /* Maximum desirable number of incoming `rev` messages that aren't being handled yet.
        Past this number, the puller will stop handling or responding to `changes` messages,
        to attempt to stop getting more `revs`.
        Can be overridden by the replicator option \ref kC4ReplicatorOptionMaxRevsBeingRequested */
    constexpr unsigned kDefaultMaxRevsBeingRequested = 200;

    /* Maximum number of simultaneous incoming revisions.
        Each one is assigned an IncomingRev actor, so larger values increase memory usage
        and also parallelism.
        Can be overridden by the replicator option \ref kC4ReplicatorOptionMaxIncomingRevs */
    constexpr unsigned kDefaultMaxIncomingRevs = 200;

    /* Maximum number of incoming revisions that haven't yet been inserted into the database
        (and are thus holding onto the document bodies in memory.) */
    constexpr unsigned kMaxActiveIncomingRevs = 100;


    //// Pusher:

    /* If true, `changes` messages are sent in BLIP Urgent mode, which means they get
            prioritized over other messages, reducing their latency. This helps keep the pusher
            from getting starved of revs to send. */
    constexpr bool kChangeMessagesAreUrgent = true;

    /* How many changes messages can be active at once */
    constexpr unsigned kMaxChangeListsInFlight = 5;

    /* Max desirable number of revs waiting to be sent. Past this number, the Pusher will
            stop querying for more lists of changes. */
    constexpr unsigned kMaxRevsQueued = 600;

    /* Max # of `rev` messages to be transmitting at once.
        Can be overridden by the replicator option \ref kC4ReplicatorOptionMaxRevsInFlight */
    constexpr unsigned kDefaultMaxRevsInFlight = 10;

    /* Max desirable number of bytes of revisions that have been sent but not replied to
            yet. This is limited to avoid flooding the peer with too much JSON data. */
    constexpr unsigned kMaxRevBytesAwaitingReply = 2 * 1024 * 1024;

    /* Number of changes to send in one "changes" msg */
    constexpr unsigned kDefaultChangeBatchSize = 200;

    /* Max history length to use, if "changes" response doesn't have one */
    constexpr unsigned kDefaultMaxHistory = 50;


    //// Replicator:

    /* How often to save checkpoints. */
    static constexpr auto kDefaultCheckpointSaveDelay = 5s;

    /* How long to wait between delegate calls notifying that that docs have finished. */
    constexpr auto kMinDocEndedInterval = 200ms;

    /* How long to wait between delegate calls when only the progress % has changed. */
    constexpr auto kMinDelegateCallInterval = 200ms;
}  // namespace litecore::repl::tuning
