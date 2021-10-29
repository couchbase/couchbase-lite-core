//
//  access_lock.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/19.
//  Copyright 2019-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include "RefCounted.hh"
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace litecore {

    /** A wrapper that protects an object from being used re-entrantly.
        It owns the object and a mutex, and exposes the object only while the mutex is locked.
        The object can be accessed in two ways:
        1.  Through a temporary `access` proxy object returned by `useLocked()` (no args).
            The proxy dereferences to the owned object via the `->` and `*` operators, or by
            implicit conversion.
            The mutex is locked when the proxy is created, and unlocked by its destructor.
            Thanks to C++ scope rules, if you use `useLocked()` as a parameter or receiver of
            a call, the mutex stays locked until the call completes.
            It's also valid to assign `useLocked()` to an auto variable and then use that one or
            more times; the mutex will remain locked until that variable exits scope.
        2.  Through a lambda/callback passed to the `useLocked()` flavors that take a parameter.
            This locks the mutex, passes a reference to the owned object to the lambda, then
            unlocks the mutex. */
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
        using SENTRY = std::function<void(const T&)>;

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
            friend access_lock;
            access(MUTEX &mut, REF ref, SENTRY* pSentry):access(mut, ref) {
                if (*pSentry) (*pSentry)(_ref);
            }
            access_lock::LOCK_GUARD _lock;
            REF                     _ref;
        };

        template <class R> using Retained = fleece::Retained<R>;

        // (specialization when T is Retained<>)
        template <class R>
        class access<Retained<R>&> {
        public:
            access(MUTEX &mut, Retained<R> &ref)     :_lock(mut), _ref(ref) { }
            access(const access&) =delete;  // I cannot be copied
#if defined(_MSC_VER) && _MSC_VER < 1920
            // https://developercommunity.visualstudio.com/t/15935-guaranteed-copy-elision-failure/1398603
            // Deleting this constructor causes copy elision failure.  Microsoft won't fix prior to VS 2019.
            access(access&&) { throw std::runtime_error("No moving!"); }
#else
            access(access&&) = delete;       // I cannot be moved
#endif

            Retained<R>& get()              {return _ref;}
            operator R* ()                  {return _ref;}
            R* operator -> ()               {return _ref;}

        private:
            friend access_lock;
            access(MUTEX &mut, Retained<R> &ref, SENTRY *pSentry):access(mut, ref) {
                if (*pSentry) (*pSentry)(_ref);
            }
            access_lock::LOCK_GUARD _lock;
            Retained<R>&            _ref;
        };

        // (specialization for const access when T is Retained<>)
        template <class R>
        class access<const Retained<R>&> {
        public:
            access(MUTEX &mut, const R* ref):_lock(mut), _ref(ref) { }
            access(const access&) =delete;  // I cannot be copied
#if defined(_MSC_VER) && _MSC_VER < 1920
            // https://developercommunity.visualstudio.com/t/15935-guaranteed-copy-elision-failure/1398603
            // Deleting this constructor causes copy elision failure.  Microsoft won't fix prior to VS 2019.
            access(access&&) { throw std::runtime_error("No moving!"); }
#else
            access(access&&) = delete;       // I cannot be moved
#endif

            auto get()                      {return _ref;}
            operator const R* ()            {return _ref;}
            auto operator * ()              {return _ref;}
            auto operator -> ()             {return _ref;}

        private:
            friend access_lock;
            access(MUTEX &mut, const R* ref, SENTRY *pSentry):access(mut, ref) {
                if (*pSentry) (*pSentry)(Retained<R>(const_cast<R*>(_ref)));
            }
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
        auto useLocked() {
            return access<T&>(_mutex, _contents, &_sentry);
        }

        /// Locks my mutex and passes a refence to my contents to the callback.
        template <class LAMBDA>
        void useLocked(LAMBDA callback) {
            LOCK_GUARD lock(_mutex);
            if (_sentry) _sentry(_contents);
            callback(_contents);
        }

        /// Locks my mutex and passes a refence to my contents to the callback.
        /// The callback's return value will be passed along and returned from this call.
        /// Due to C++'s limited type inference, the RESULT type has to be specified explicitly,
        /// e.g. `useLocked<actualResultClass>(...)`
        template <class RESULT, class LAMBDA>
        RESULT useLocked(LAMBDA callback) {
            LOCK_GUARD lock(_mutex);
            if (_sentry) _sentry(_contents);
            return callback(_contents);
        }

        // const versions:

        auto useLocked() const  {
            return access<const T&>(getMutex(), _contents, const_cast<SENTRY*>(&_sentry));
        }

        template <class LAMBDA>
        void useLocked(LAMBDA callback) const {
            LOCK_GUARD lock(getMutex());
            if (_sentry) _sentry(_contents);
            callback(_contents);
        }

        template <class RESULT, class LAMBDA>
        RESULT useLocked(LAMBDA callback) const {
            LOCK_GUARD lock(getMutex());
            if (_sentry) _sentry(_contents);
            return callback(_contents);
        }


        MUTEX& getMutex() const {return const_cast<MUTEX&>(_mutex);}

    protected:
        SENTRY _sentry;

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
