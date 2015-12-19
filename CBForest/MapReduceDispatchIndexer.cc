//
//  MapReduceDispatchIndexer.cc
//  CBForest
//
//  Created by Jens Alfke on 7/28/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "MapReduceDispatchIndexer.hh"
#include <dispatch/dispatch.h>


namespace cbforest {

    MapReduceDispatchIndexer::MapReduceDispatchIndexer(std::vector<MapReduceIndex*> indexes,
                                                       Transaction& transaction,
                                                       dispatch_queue_priority_t priority)
    :MapReduceIndexer(indexes, transaction),
     _queue(dispatch_get_global_queue(priority, 0))
    { }

    void MapReduceDispatchIndexer::addMappable(const Mappable& mappable) {
        ::dispatch_apply(indexCount(), _queue, ^(size_t i) {
            updateDocInIndex(i, mappable);
        });
    }

    MapReduceDispatchIndexer::~MapReduceDispatchIndexer() {
        // Save each index's state, and delete the transactions:
        ::dispatch_apply(_transactions.size(), _queue, ^(size_t i) {
            if (_transactions[i]) {
                if (_finished)
                    _indexes[i]->saveState(*_transactions[i]);
                delete _transactions[i];
            }
        });
        _transactions.resize(0);
    }

}
