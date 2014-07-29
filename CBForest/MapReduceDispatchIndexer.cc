//
//  MapReduceDispatchIndexer.cc
//  CBForest
//
//  Created by Jens Alfke on 7/28/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "MapReduceDispatchIndexer.hh"
#include <dispatch/dispatch.h>


namespace forestdb {

    MapReduceDispatchIndexer::MapReduceDispatchIndexer(std::vector<MapReduceIndex*> indexes,
                                                       qos_class_t priority)
    :MapReduceIndexer(indexes),
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
