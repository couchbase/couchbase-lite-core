//
//  RefCounted.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "Error.hh"
#include <atomic>

namespace CBL_Core {

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
    template <typename SELF>
    struct RefCounted : InstanceCounted {

        int refCount() const { return _refCount; }

        SELF* retain() {
            ++_refCount;
            return (SELF*)this;
        }

        void release() {
            int newref = --_refCount;
            CBFAssert(newref >= 0);
            if (newref == 0) {
                delete this;
            }
        }

    protected:
        /** Destructor is accessible only so that it can be overridden.
            Never call delete, only release! */
        virtual ~RefCounted() {
            CBFAssert(_refCount == 0);
        }

    private:
        std::atomic_int _refCount {0};
    };


    /** Simple smart pointer that retains the RefCounted instance it holds. */
    template <typename T>
    class Retained {
    public:
        Retained(T *t)          :_ref(t->retain()) { }
        ~Retained()             {_ref->release();}
        operator T* () const    {return _ref;}
        T* operator-> () const  {return _ref;}
    private:
        T *_ref;

        Retained(const Retained&) =delete;
        Retained& operator=(const Retained&) =delete;
    };

}
