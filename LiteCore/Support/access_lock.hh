//
//  access_lock.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include <functional>
#include <mutex>
#include <type_traits>


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

        using SENTRY = std::function<void(const T&)>;

        template <class LAMBDA>
        void use(LAMBDA callback) {
            LOCK lock(_mutex);
            if (_sentry) _sentry(_contents);
            callback(_contents);
        }

        // Returns result. Has to be called as `use<actualResultClass>(...)`
        template <class RESULT, class LAMBDA>
        RESULT use(LAMBDA callback) {
            LOCK lock(_mutex);
            if (_sentry) _sentry(_contents);
            return callback(_contents);
        }

        // const versions:

        template <class LAMBDA>
        void use(LAMBDA callback) const {
            LOCK lock(_mutex);
            if (_sentry) _sentry(_contents);
            callback(_contents);
        }

        // Returns result. Has to be called as `use<actualResultClass>(...)`
        template <class RESULT, class LAMBDA>
        RESULT use(LAMBDA callback) const {
            LOCK lock(getMutex());
            if (_sentry) _sentry(_contents);
            return callback(_contents);
        }

        MUTEX& getMutex() const {return const_cast<MUTEX&>(_mutex);}

    protected:
        SENTRY _sentry;

    private:
        using LOCK = std::lock_guard<std::remove_reference_t<MUTEX>>;

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
