//
//  RefCounted.cc
//  CBForest
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "RefCounted.hh"

namespace cbforest {

    std::atomic_int InstanceCounted::gObjectCount;

}
