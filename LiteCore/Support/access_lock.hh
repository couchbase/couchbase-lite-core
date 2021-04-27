//
//  access_lock.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include <mutex>
#include <type_traits>
#include <utility>

namespace litecore {

    /** A wrapper that protects an object from being used re-entrantly.
        The object is only accessible through a callback, and an internal mutex prevents
        multiple callbacks from running at once. */
    template <class T, class MUTEX =std::recursive_mutex>
    class access_lock {
    public:
        access_lock()
        :_contents()
        { }

        explicit access_lock(T &&contents)
        :_contents(std::move(contents))
        { }

        explicit access_lock(T &&contents, MUTEX &mutex)
        :_contents(std::move(contents))
        ,_mutex(mutex)
        { }

        using LOCK_GUARD = std::lock_guard<std::remove_reference_t<MUTEX>>;


        /** Temporary access token returned by the `useLocked()` method. You can pass it as a parameter
            as though it were that object, call the object via `->`, or access the object
            explicitly via `get()`. */
        template <class REF>
        class access {
        public:
            access(MUTEX &mut, REF ref)     :_lock(mut), _ref(ref) { }
            access(const access&) =delete;  // I cannot be copied
#if defined(_MSC_VER) && _MSC_VER < 1920
            // https://developercommunity.visualstudio.com/t/15935-guaranteed-copy-elision-failure/1398603
            // Deleting this constructor causes copy elision failure.  Microsoft won't fix prior to VS 2019.
            access(access&&) { throw std::runtime_error("No moving!"); }
#else
            access(access&&) = delete;       // I cannot be moved
#endif

            REF get()                       {return _ref;}
            operator REF ()                 {return _ref;}
            auto operator -> ()             {return &_ref;}

        private:
            access_lock::LOCK_GUARD _lock;
            REF                     _ref;
        };

        template <class R> using Retained = fleece::Retained<R>;

        template <class R>
        class access<Retained<R>&> {
        public:
            access(MUTEX &mut, Retained<R> &ref)     :_lock(mut), _ref(ref) { }
            access(const access&) =delete;  // I cannot be copied
            access(access&&) { throw std::runtime_error("No moving!"); }      // I cannot be moved

            Retained<R>& get()              {return _ref;}
            operator R* ()                  {return _ref;}
            R* operator -> ()               {return _ref;}

        private:
            access_lock::LOCK_GUARD _lock;
            Retained<R>&            _ref;
        };

        template <class R>
        class access<const Retained<R>&> {
        public:
            access(MUTEX &mut, const R* ref):_lock(mut), _ref(ref) { }
            access(const access&) =delete;  // I cannot be copied
            access(access&&) { throw std::runtime_error("No moving!"); }       // I cannot be moved

            auto get()                      {return _ref;}
            operator const R* ()            {return _ref;}
            auto operator * ()              {return _ref;}
            auto operator -> ()             {return _ref;}

        private:
            access_lock::LOCK_GUARD _lock;
            const R*                _ref;
        };

        
        /// Locks my mutex and returns an `access` object that acts as a proxy for my contents.
        /// My mutex is unlocked in the `access` object's destructor.
        /// If you use a `useLocked()` call as a function parameter, this will do the right thing and
        /// unlock the mutex after that function returns.
        /// You can assign the result of `useLocked()` to a local variable and use it multiple times
        /// within the same lock scope; just make sure that variable has as brief a lifetime as
        /// possible.
        auto useLocked()        {return access<T&>(_mutex, _contents);}

        /// Locks my mutex and passes a refence to my contents to the callback.
        template <class LAMBDA>
        void useLocked(LAMBDA callback) {
            LOCK_GUARD lock(_mutex);
            callback(_contents);
        }

        /// Locks my mutex and passes a refence to my contents to the callback.
        /// The callback's return value will be passed along and returned from this call.
        /// Due to C++'s limited type inference, the RESULT type has to be specified explicitly,
        /// e.g. `useLocked<actualResultClass>(...)`
        template <class RESULT, class LAMBDA>
        RESULT useLocked(LAMBDA callback) {
            LOCK_GUARD lock(_mutex);
            return callback(_contents);
        }

        // const versions:

        auto useLocked() const  {return access<const T&>(getMutex(), _contents);}

        template <class LAMBDA>
        void useLocked(LAMBDA callback) const {
            LOCK_GUARD lock(getMutex());
            callback(_contents);
        }

        template <class RESULT, class LAMBDA>
        RESULT useLocked(LAMBDA callback) const {
            LOCK_GUARD lock(getMutex());
            return callback(_contents);
        }


        MUTEX& getMutex() const {return const_cast<MUTEX&>(_mutex);}

    private:
        T _contents;
        MUTEX _mutex;
    };


    /** An access_lock that shares the same mutex as another instance instead of having its own.
        Obviously the other instance needs to remain valid as long as this one exists! */
    template <class T, class MUTEX =std::recursive_mutex>
    class shared_access_lock : public access_lock<T, MUTEX&> {
    public:
        template <class U>
        explicit shared_access_lock(T &&contents, const access_lock<U,MUTEX> &sharing)
        :access_lock<T, MUTEX&>(std::move(contents), sharing.getMutex())
        { }

        template <class U>
        explicit shared_access_lock(T &&contents, const access_lock<U,MUTEX> *sharing)
        :access_lock<T, MUTEX&>(std::move(contents), sharing->getMutex())
        { }
    };

}
