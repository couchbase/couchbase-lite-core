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
#include "RefCounted.hh"
#include <shared_mutex>

namespace litecore {

/** WeakHolder<T>: holds a pointer to T weakly. Unlike general weak reference, one cannot get a strong holder from it.
    Instead, we can call the methods of class T via invoke, which returns true if the call goes through as the underlying
    pointer is good. */
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

    /** Call the member function with the underlying pointer.
        @param memFuncPtr pointer to the member function.
        @param args arguments passed to the member function.
        @return true if the underlying pointer is good, and false otherwise.
        @warning what is returned from the member fundtion, if not void, will be thrown away. */
    template<typename MemFuncPtr, typename ... Args>
    bool invoke(MemFuncPtr memFuncPtr, Args&& ... args) {
        std::shared_lock<std::shared_mutex> shl(_mutex);
        if (_pointer == nullptr) {
            return false;
        }
        (_pointer->*memFuncPtr)(std::forward<Args>(args)...);
        return true;
    }

private:
    T*    _pointer;
    std::shared_mutex _mutex;
};

}
