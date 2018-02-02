//
// RefCounted.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include <atomic>

namespace litecore {

    /** Simple thread-safe ref-counting implementation.
        Note: The ref-count starts at 0, so you must call retain() on an instance, or assign it
        to a Retained, right after constructing it. */
    class RefCounted {
    public:

        int refCount() const                    { return _refCount; }

    protected:
        /** Destructor is accessible only so that it can be overridden.
            Never call delete, only release! */
        virtual ~RefCounted();

    private:
        template <typename T>
        friend T* retain(T*) noexcept;
        friend void release(RefCounted*) noexcept;

#if DEBUG
        void _retain() noexcept;
        void _release() noexcept;
        static constexpr int32_t kInitialRefCount = -66666;
#else
        inline void _retain() noexcept          { ++_refCount; }
        inline void _release() noexcept         { if (--_refCount <= 0) delete this; }
        static constexpr int32_t kInitialRefCount = 0;
#endif

        std::atomic<int32_t> _refCount {kInitialRefCount};
    };


    /** Retains a RefCounted object and returns the object. Does nothing given a null pointer. */
    template <typename REFCOUNTED>
    inline REFCOUNTED* retain(REFCOUNTED *r) noexcept {
        if (r) r->_retain();
        return r;
    }

    /** Releases a RefCounted object. Does nothing given a null pointer. */
    inline void release(RefCounted *r) noexcept {
        if (r) r->_release();
    }


    /** Simple smart pointer that retains the RefCounted instance it holds. */
    template <typename T>
    class Retained {
    public:
        Retained() noexcept                      :_ref(nullptr) { }
        Retained(T *t) noexcept                  :_ref(retain(t)) { }
        Retained(const Retained &r) noexcept     :_ref(retain(r._ref)) { }
        Retained(Retained &&r) noexcept          :_ref(r._ref) {r._ref = nullptr;}
        ~Retained()                              {release(_ref);}

        operator T* () const noexcept            {return _ref;}
        T* operator-> () const noexcept          {return _ref;}
        T* get() const noexcept                  {return _ref;}

        Retained& operator=(T *t) noexcept {
            auto oldRef = _ref;
            _ref = retain(t);
            release(oldRef);
            return *this;
        }

        Retained& operator=(const Retained &r) noexcept {
            return *this = r._ref;
        }

        Retained& operator= (Retained &&r) noexcept {
            auto oldRef = _ref;
            _ref = r._ref;
            r._ref = nullptr;
            release(oldRef);
            return *this;
        }

    private:
        T *_ref;
    };


    template <typename REFCOUNTED>
    inline Retained<REFCOUNTED> retained(REFCOUNTED *r) noexcept {
        return Retained<REFCOUNTED>(r);
    }


}
