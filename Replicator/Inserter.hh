//
// Inserter.hh
//
//  Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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

        bool passive(unsigned collectionIndex =0) const override {
            return _options->pullOf(collectionIndex) <= kC4Passive;
        }

    private:
        void _insertRevisionsNow(int gen);
        bool insertRevisionNow(RevToInsert* NONNULL, C4Error*);
        C4SliceResult applyDeltaCallback(C4Document *doc NONNULL,
                                         C4Slice deltaJSON,
                                         C4Error *outError);

        actor::ActorBatcher<Inserter,RevToInsert> _revsToInsert; // Pending revs to be added to db
    };

} }
