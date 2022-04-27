//
//  WeakHolder.hh
//
//  Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/RefCounted.hh"
#include <shared_mutex>

namespace litecore {

template <typename T>
class WeakHolder : public RefCounted {
public:
    WeakHolder(T* pointer)
    : _pointer(pointer)
    {
        DebugAssert(_pointer != nullptr);
    }

    // Only the original owner may rescind the pointer.
    // After rescind(), the held pointer becomes nullptr and invoke() returns false.
    void rescind(T* owner) {
        if (owner == _pointer) {
            std::lock_guard<std::shared_mutex> lock(_mutex);
            _pointer = nullptr;
        }
    }

    template<typename Callable, typename ... Args>
    bool invoke(Callable fun, Args&& ... args) {
        std::shared_lock<std::shared_mutex> shl(_mutex);
        if (_pointer == nullptr) {
            return false;
        }
        fun(_pointer, std::forward<Args>(args)...);
        return true;
    }

private:
    T*    _pointer;
    std::shared_mutex _mutex;
};

}
