//
// RevFinder.cc
//
//  Copyright (c) 2019 Couchbase. All rights reserved.
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
#include "Replicator.hh"
#include <vector>

namespace litecore { namespace repl {
    class DocIDMultiset;

    /** Used by Puller to check which revisions in a "revs" message are new and should be
        pulled, and send the response to the message. */
    class RevFinder : public Worker {
    public:
        RevFinder(Replicator*);

        /** Asynchronously processes a "revs" message, sends the response, and calls the
            completion handler with a bit-vector indicating which revs were requested. */
        void findOrRequestRevs(blip::MessageIn *msg NONNULL,
                               DocIDMultiset *incomingDocs NONNULL,
                               std::function<void(std::vector<bool>)> completion)
        {
            enqueue(&RevFinder::_findOrRequestRevs, retained(msg), incomingDocs, completion);
        }

        void onError(C4Error err) override;

    private:
        static const size_t kMaxPossibleAncestors = 10;

        void _findOrRequestRevs(Retained<blip::MessageIn>,
                                DocIDMultiset *incomingDocs,
                                std::function<void(std::vector<bool>)> completion);
        int findProposedChange(slice docID, slice revID, slice parentRevID,
                               alloc_slice &outCurrentRevID);

        bool _announcedDeltaSupport {false};                // Did I send "deltas:true" yet?
    };

} }
