//
// Inserter.hh
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
#include "Worker.hh"
#include "Batcher.hh"

namespace litecore { namespace repl {
    class Replicator;
    class RevToInsert;

    /** Inserts revisions into the database in batches. */
    class Inserter : public Worker {
    public:
        Inserter(Replicator*);

        void insertRevision(RevToInsert* NONNULL);

    private:
        void _insertRevisionsNow(int gen);
        bool insertRevisionNow(RevToInsert* NONNULL, C4Error*);
        C4SliceResult applyDeltaCallback(C4Document *doc NONNULL,
                                         C4Slice deltaJSON,
                                         C4Error *outError);

        actor::ActorBatcher<Inserter,RevToInsert> _revsToInsert; // Pending revs to be added to db
    };

} }
