//
//  MapReduceDispatchIndexer.h
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

#ifndef __CBForest__MapReduceDispatchIndexer__
#define __CBForest__MapReduceDispatchIndexer__

#include "MapReduceIndex.hh"
#import <dispatch/dispatch.h>


namespace cbforest {

    /** MapReduceIndexer that uses dispatch queues (GCD) to parallelize running map functions. */
    class MapReduceDispatchIndexer : public MapReduceIndexer {
    public:
        MapReduceDispatchIndexer(std::vector<MapReduceIndex*> indexes,
                                 Transaction&,
                                 dispatch_queue_priority_t = DISPATCH_QUEUE_PRIORITY_DEFAULT);
        virtual ~MapReduceDispatchIndexer();
    protected:
        virtual void addMappable(const Mappable&);

    private:
        dispatch_queue_t _queue;
    };

}

#endif /* defined(__CBForest__MapReduceDispatchIndexer__) */
