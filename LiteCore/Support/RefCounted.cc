//
//  RefCounted.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "RefCounted.hh"
#include "Logging.hh"
#include <exception>
#include <stdlib.h>

namespace litecore {

    static void fail(RefCounted *obj, const char *what, int refCount) {
        char message[100];
        sprintf(message, "RefCounted object at %p %s while it had an invalid refCount of %d",
             obj, what, refCount);
        WarnError("%s", message);
        throw std::runtime_error(message);
    }

    RefCounted::~RefCounted() {
        int32_t ref = _refCount;
        if (ref != 0)
            fail(this, "destructed", ref);
#if DEBUG
        // Store a garbage value to detect use-after-free
        _refCount = 0xDDDDDDDD;
#endif
    }

#if DEBUG
    // In debug builds, a simple check for a garbage refcount, to detect if I've been
    // deleted or corrupted.

    void RefCounted::_retain() noexcept {
        auto ref = ++_refCount;
        if (ref <= 0 || ref >= 10000000)
            fail(this, "retained", ref);
    }

    void RefCounted::_release() noexcept {
        auto ref = --_refCount;
        if (ref+1 <= 0 || ref+1 >= 10000000)
            fail(this, "released", ref+1);
        if (ref <= 0) delete this;
    }
#endif
    
}
