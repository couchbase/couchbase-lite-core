//
//  MapReduceDispatchIndexer.h
//  CBForest
//
//  Created by Jens Alfke on 7/28/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__MapReduceDispatchIndexer__
#define __CBForest__MapReduceDispatchIndexer__

#include "MapReduceIndex.hh"
#import <dispatch/dispatch.h>
#import <sys/qos.h>


namespace forestdb {

    /** MapReduceIndexer that uses dispatch queues (GCD) to parallelize running map functions. */
    class MapReduceDispatchIndexer : public MapReduceIndexer {
    public:
        MapReduceDispatchIndexer(std::vector<MapReduceIndex*> indexes,
                                 qos_class_t priority = QOS_CLASS_UTILITY);
        virtual ~MapReduceDispatchIndexer();
    protected:
        virtual void addMappable(const Mappable&);

    private:
        dispatch_queue_t _queue;
    };

}

#endif /* defined(__CBForest__MapReduceDispatchIndexer__) */
