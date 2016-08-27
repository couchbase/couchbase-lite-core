//
//  RefCounted.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#include "RefCounted.hh"

namespace CBL_Core {

    std::atomic_int InstanceCounted::gObjectCount;

}
