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
        friend T* retain(T*);
        friend void release(RefCounted*);

        inline void _retain() noexcept          { ++_refCount; }
        inline void _release() noexcept         {if (--_refCount <= 0) dealloc();}
        void dealloc() noexcept;

        std::atomic<int32_t> _refCount {0};
    };


    /** Retains a RefCounted object and returns the object. */
    template <typename REFCOUNTED>
    inline REFCOUNTED* retain(REFCOUNTED *r) {
        if (r) r->_retain();
        return r;
    }

    /** Retains a RefCounted object and returns the object. */
    inline void release(RefCounted *r) {
        if (r) r->_release();
    }


    /** Simple smart pointer that retains the RefCounted instance it holds. */
    template <typename T>
    class Retained {
    public:
        Retained()                      :_ref(nullptr) { }
        Retained(T *t)                  :_ref(retain(t)) { }
        Retained(const Retained &r)     :_ref(retain(r._ref)) { }
        Retained(Retained &&r)          :_ref(r._ref) {r._ref = nullptr;}
        ~Retained()                     {release(_ref);}

        operator T* () const            {return _ref;}
        T* operator-> () const          {return _ref;}
        T* get() const                  {return _ref;}

        Retained& operator=(const Retained &r) {
            release(_ref);
            _ref = r._ref;
            retain(_ref);
            return *this;
        }

        Retained& operator= (Retained &&r) {
            release(_ref);
            _ref = r._ref;
            r._ref = nullptr;
            return *this;
        }

    private:
        T *_ref;
    };

}
