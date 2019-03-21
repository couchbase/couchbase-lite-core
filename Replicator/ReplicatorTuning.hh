//
// ReplicatorTuning.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
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
#include "Timer.hh"

namespace litecore { namespace repl {

    /* Constants for tuning the performance of the replicator.
        These are black magic. Don't change them lightly. They have synergistic effects with
        each other, and changing them can have unexpected and counter-intuitive effects.
        Their behavior also varies with things like network speed, latency, and whether the
        peer is LiteCore or Sync Gateway.
        I'm not sure the current values are optimal, but they've been tweaked a lot. --Jens */
    namespace tuning {

        //// DBWorker:

        /* Number of new revisions to accumulate in memory before inserting them into the DB.
           (Actually the queue may grow larger than this, since the insertion is triggered
           asynchronously, and more revs may be added to the queue before it happens.) */
        constexpr size_t kInsertionBatchSize = 100;

        /* How long revisions can stay in the queue before triggering insertion into the DB,
           if the queue size hasn't reached kInsertionBatchSize yet. */
        constexpr actor::Timer::duration kInsertionDelay = std::chrono::milliseconds(20);

        /* Minimum document body size that will be considered for delta compression.
            (This is the size of the Fleece encoding, which is usually smaller than the JSON.)
           This is not declared `constexpr`, so that the delta-sync unit tests can change it. */
        extern size_t kMinBodySizeForDelta; // = 200;

        //// Puller:

        /* Number of revisions the peer should include in a single `changes` / `proposeChanges`
            message. (This is sent as a parameter in the puller's opening `subChanges` message.) */
        constexpr unsigned kChangesBatchSize = 200;

        /* Maximum desirable number of incoming `rev` messages that aren't being handled yet.
            Past this number, the puller will stop handling or responding to `changes` messages,
            to attempt to stop getting more `revs`. */
        constexpr unsigned kMaxPendingRevs = 200;

        /* Maximum number of incoming revisions to be reading/inserting at once.
            Each one is assigned an IncomingRev actor, so larger values increase memory usage
            and also parallelism (which can be bad: on Apple platforms, having too many active
            GCD dispatch queues results in lots of threads being created.) */
        constexpr unsigned kMaxActiveIncomingRevs = 100;

        constexpr unsigned kMaxUnfinishedIncomingRevs = 200;


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

        /* Max # of `rev` messages to be transmitting at once. */
        constexpr unsigned kMaxRevsInFlight = 10;

        /* Max desirable number of bytes of revisions that have been sent but not replied to
            yet. This is limited to avoid flooding the peer with too much JSON data. */
        constexpr unsigned kMaxRevBytesAwaitingReply = 2*1024*1024;

        //// Replicator:

        /* How long to wait between delegate calls notifying that that docs have finished. */
        constexpr actor::Timer::duration kMinDocEndedInterval = std::chrono::milliseconds(200);

        /* How long to wait between delegate calls when only the progress % has changed. */
        constexpr double kMinDelegateCallInterval = 0.2;
    }

} }
