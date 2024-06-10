//
// Channel.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Error.hh"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

#if __APPLE__ && !defined(ACTORS_USE_GCD)
// Use GCD if available, as it's more efficient and has better integration with OS & debugger.
#    define ACTORS_USE_GCD
#endif

// TODO: Add developer switch / debug mode to enable these options at runtime
// Set to 1 to have Actor object report performance statistics in their destructors
#define ACTORS_TRACK_STATS 0

// Set to 1 to have Actor objects track their calls through manifests to provide an
// async stack trace on exception
#define ACTORS_USE_MANIFESTS 0

namespace litecore::actor {

    /** A simple thread-safe producer/consumer queue. */
    template <class T>
    class Channel {
      public:
        /** Pushes a new value to the front of the queue.
            @return  True if the queue was empty before the push. */
        bool push(const T& t);

        /** Pops the next value from the end of the queue.
            If the queue is empty, blocks until another thread adds something to the queue.
            If the queue is closed and empty, returns a default (zero) T.
            @param empty  Will be set to true if the queue is now empty. */
        T pop(bool& empty) { return pop(empty, true); }

        /** Pops the next value from the end of the queue.
            If the queue is empty, immediately returns a default (zero) T.
            @param empty  Will be set to true if the queue is now empty. */
        T popNoWaiting(bool& empty) { return pop(empty, false); }

        /** Pops the next value from the end of the queue.
            If the queue is empty, blocks until another thread adds something to the queue.
            If the queue is closed and empty, returns a default (zero) T. */
        T pop() {
            bool more;
            return pop(more);
        }

        /** Returns the front item of the queue without popping it. The queue MUST be non-empty. */
        const T& front() const;

        /** Returns the number of items in the queue. */
        size_t size() const;

        /** When the queue is closed, after it empties all pops will return immediately with a 
            default T value instead of blocking. */
        void close();

      protected:
        mutable std::mutex _mutex;

      private:
        T pop(bool& empty, bool wait);

        std::condition_variable _cond;
        std::queue<T>           _queue;
        bool                    _closed{false};
    };

    template <class T>
    bool Channel<T>::push(const T& t) {
        std::unique_lock<std::mutex> lock(_mutex);
        bool                         wasEmpty = _queue.empty();
        if ( !_closed ) { _queue.push(t); }
        lock.unlock();

        _cond.notify_one();
        return wasEmpty;
    }

    template <class T>
    T Channel<T>::pop(bool& empty, bool wait) {
        std::unique_lock<std::mutex> lock(_mutex);
        while ( wait && _queue.empty() && !_closed ) _cond.wait(lock);
        if ( _queue.empty() ) {
            empty = true;
            return T();
        } else {
            T t(std::move(_queue.front()));
            _queue.pop();
            empty = _queue.empty();
            return t;
        }
    }

    template <class T>
    const T& Channel<T>::front() const {
        std::unique_lock<std::mutex> lock(_mutex);
        DebugAssert(!_queue.empty());
        return _queue.front();
    }

    template <class T>
    size_t Channel<T>::size() const {
        std::unique_lock<std::mutex> lock(_mutex);
        return _queue.size();
    }

    template <class T>
    void Channel<T>::close() {
        std::unique_lock<std::mutex> lock(_mutex);
        if ( !_closed ) {
            _closed = true;
            _cond.notify_all();
        }
    }

}  // namespace litecore::actor
