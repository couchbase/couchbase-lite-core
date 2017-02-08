//
//  RefCounted.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include <atomic>

namespace litecore {

    /** Base class that keeps track of the total instance count of all subclasses,
        which is returned by c4_getObjectCount(). */
    class InstanceCounted {
    public:
        static std::atomic_int gObjectCount;
        InstanceCounted()   {++gObjectCount;}
        ~InstanceCounted()  {--gObjectCount;}
    };


    /** Simple thread-safe ref-counting implementation.
        Note: The ref-count starts at 0, so you must call retain() on an instance right after
        constructing it. */
    class RefCounted {
    public:

        int refCount() const                    { return _refCount; }

        /** Destructor is accessible only so that it can be overridden.
            Never call delete, only release! */
        virtual ~RefCounted();

    private:
        template <typename T>
        friend T* retain(T*) noexcept;
        friend void release(RefCounted*) noexcept;

        inline void _retain() noexcept          { ++_refCount; }
        inline void _release() noexcept         {if (--_refCount <= 0) delete this;}

        std::atomic<int32_t> _refCount {0};
    };


    /** Retains a RefCounted object and returns the object. */
    template <typename REFCOUNTED>
    inline REFCOUNTED* retain(REFCOUNTED *r) noexcept {
        if (r) r->_retain();
        return r;
    }

    /** Retains a RefCounted object and returns the object. */
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
            retain(t);
            release(_ref);
            _ref = t;
            return *this;
        }

        Retained& operator=(const Retained &r) noexcept {
            return *this = r._ref;
        }

        Retained& operator= (Retained &&r) noexcept {
            release(_ref);
            _ref = r._ref;
            r._ref = nullptr;
            return *this;
        }

    private:
        T *_ref;
    };

}
