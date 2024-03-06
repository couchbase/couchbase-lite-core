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

namespace litecore::repl {
    class Replicator;
    class RevToInsert;

    /** Inserts revisions into the database in batches. */
    class Inserter : public Worker {
      public:
        Inserter(Replicator*, CollectionIndex);

        void insertRevision(RevToInsert* NONNULL);

        bool passive() const override { return _options->pull(collectionIndex()) <= kC4Passive; }

      private:
        C4Collection* insertionCollection();  // Get the collection from the insertionDB

        void          _insertRevisionsNow(int gen);
        bool          insertRevisionNow(RevToInsert* NONNULL, C4Error*);
        C4SliceResult applyDeltaCallback(C4Document* doc NONNULL, C4Slice deltaJSON, C4RevisionFlags* revFlags,
                                         C4Error* outError);

        actor::ActorBatcher<Inserter, RevToInsert> _revsToInsert;  // Pending revs to be added to db
        C4Collection*                              _insertionCollection{nullptr};
    };

}  // namespace litecore::repl
