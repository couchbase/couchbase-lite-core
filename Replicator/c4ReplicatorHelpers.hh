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

namespace litecore { namespace repl {

    struct C4ReplParamsDefaultCollection : C4ReplicatorParameters {
        C4ReplicationCollection defaultCollection{kC4DefaultCollectionSpec};

        C4ReplParamsDefaultCollection()
        : C4ReplicatorParameters()
        , push(defaultCollection.push)
        , pull(defaultCollection.pull)
        , pushFilter(defaultCollection.pushFilter)
        , validationFunc(defaultCollection.pullFilter)
        {
            collections = &defaultCollection;
            collectionCount = 1;
        }

        C4ReplicatorMode& push;
        C4ReplicatorMode& pull;
        C4ReplicatorValidationFunction C4NONNULL & pushFilter;
        C4ReplicatorValidationFunction C4NONNULL & validationFunc;
    };

}}
