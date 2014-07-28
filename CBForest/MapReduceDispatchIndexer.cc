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

    MapReduceDispatchIndexer::MapReduceDispatchIndexer(std::vector<MapReduceIndex*> indexes)
    :MapReduceIndexer(indexes),
     _queue(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0))
    { }

    void MapReduceDispatchIndexer::addMappable(const Mappable& mappable) {
        ::dispatch_apply(indexCount(), _queue, ^(size_t i) {
            updateDocInIndex(i, mappable);
        });
    }

}
