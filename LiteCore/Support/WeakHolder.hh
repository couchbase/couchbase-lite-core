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

namespace litecore {

/** WeakHolder<T>: holds a pointer to T weakly. Unlike general weak reference, one cannot get a strong holder from it.
    Instead, we can call the methods of class T via invoke, which returns true if pointer is strongly referenced by some
    object other than *this.
    Pre-conditions: T must be dynamically castable to RefCounted.
    Note: WeakHolder holds a strong reference but provides no API to release it. Threfore, leaking of
    WeakHolder also leaks the object being held. It also implies that you cannot rely on the destructor of the held object
    to auto-release the WeakHolder.
 */
template <typename T>
class WeakHolder : public RefCounted {
public:
    template <typename U>
    WeakHolder(U* pointer)
    : _pointer(pointer)
    {
        DebugAssert(_pointer != nullptr);
        RefCounted* refCounted = dynamic_cast<RefCounted*>(pointer);
        _holder = refCounted;
        Assert(_holder);
    }

    /** Call the member function with the underlying pointer.
        @param memFuncPtr pointer to the member function.
        @param args arguments passed to the member function.
        @return true if the underlying pointer is good, and false otherwise.
        @warning what is returned from the member fundtion, if not void, will be thrown away. */
    template<typename MemFuncPtr, typename ... Args>
    bool invoke(MemFuncPtr memFuncPtr, Args&& ... args) {
        Retained<RefCounted> holdingIt = _holder;
        if (_holder->refCount() == 2) {
            // There is no place outside here do references exist.
            return false;
        }
        (_pointer->*memFuncPtr)(std::forward<Args>(args)...);
        return true;
    }

private:
    // Invariant: dynamic_cast<RefCounted*>(_pointer) == _holder.get()
    T*    _pointer;
    Retained<RefCounted> _holder;
};

}
