//
//  Channel.cc
//  LiteCore
//
//  Created by Jens Alfke on 1/8/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Channel.hh"

namespace litecore {

    template <class T>
    bool Channel<T>::push(const T &t) {
        std::unique_lock<std::mutex> lock(_mutex);
        bool wasEmpty = _queue.empty();
        if (!_closed) {
            _queue.push(t);
        }
        lock.unlock();

        if (wasEmpty)
            _cond.notify_one();
        return wasEmpty;
    }


    /** Pops the next value from the end of the queue.
     If the queue is empty, blocks until another thread adds something to the queue.
     @param empty  Will be set to true if there are no more values remaining in the queue. */
    template <class T>
    T Channel<T>::pop(bool &empty) {
        std::unique_lock<std::mutex> lock(_mutex);
        while (_queue.empty() && !_closed)
            _cond.wait(lock);
        if (_queue.empty()) {
            empty = true;
            return T();
        } else {
            T t( std::move(_queue.front()) );
            _queue.pop();
            empty = _queue.empty();
            return t;
        }
    }


    /** Pops the next value from the end of the queue, or returns if the queue is empty.
     @return  True if a value was popped, or false if the queue was empty. */
    template <class T>
    bool Channel<T>::popNoWaiting(T &t) {
        std::unique_lock<std::mutex> lock(_mutex);
        if (_queue.empty())
            return false;
        t = std::move(_queue.front());
        _queue.pop();
        return true;
    }


    /** Returns true if the queue is currently empty. */
    template <class T>
    bool Channel<T>::empty() const {
        std::unique_lock<std::mutex> lock((std::mutex&)_mutex);
        return _queue.empty();
    }

    
    template <class T>
    void Channel<T>::close() {
        std::unique_lock<std::mutex> lock(_mutex);
        if (!_closed) {
            _closed = true;
            _cond.notify_all();
        }
    }

}
