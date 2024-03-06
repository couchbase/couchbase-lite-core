//
//  C4ReplicatorHelpers.hh
//
//  Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4ReplicatorTypes.h"

namespace litecore::repl {

    // Helper struct to make testing with collections easier
    struct C4ReplParamsOneCollection : C4ReplicatorParameters {
        C4ReplicationCollection replCollection;

        explicit C4ReplParamsOneCollection(C4CollectionSpec collectionSpec)
            : C4ReplicatorParameters{}
            , replCollection{collectionSpec}
            , push(replCollection.push)
            , pull(replCollection.pull)
            , pushFilter(replCollection.pushFilter)
            , validationFunc(replCollection.pullFilter) {
            collections     = &replCollection;
            collectionCount = 1;
        }

        C4ReplParamsOneCollection(C4CollectionSpec collectionSpec, C4ReplicatorMode pushMode, C4ReplicatorMode pullMode)
            : C4ReplParamsOneCollection(collectionSpec) {
            push = pushMode;
            pull = pullMode;
        }

        C4ReplicatorMode&                         push;
        C4ReplicatorMode&                         pull;
        C4ReplicatorValidationFunction C4NONNULL& pushFilter;
        C4ReplicatorValidationFunction C4NONNULL& validationFunc;
    };

    struct C4ReplParamsDefaultCollection : C4ReplParamsOneCollection {
        C4ReplParamsDefaultCollection() : C4ReplParamsOneCollection{kC4DefaultCollectionSpec} {}
    };

}  // namespace litecore::repl
