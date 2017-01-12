//
//  Channel.hh
//  LiteCore
//
//  Created by Jens Alfke on 1/8/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include <condition_variable>
#include <mutex>
#include <queue>

namespace litecore {

    /** A simple thread-safe producer/consumer queue. */
    template <class T>
    class Channel {
    public:

        /** Pushes a new value to the front of the queue.
            @return  True if the queue was empty before the push. */
        bool push(const T &t);

        /** Pops the next value from the end of the queue.
            If the queue is empty, blocks until another thread adds something to the queue.
            @param empty  Will be set to true if there are no more values remaining in the queue. */
        T pop(bool &empty);

        /** Pops the next value from the end of the queue.
            If the queue is empty, blocks until another thread adds something to the queue. */
        T pop() {
            bool more;
            return pop(more);
        }

        /** Pops the next value from the end of the queue, or returns if the queue is empty.
            @return  True if a value was popped, or false if the queue was empty. */
        bool popNoWaiting(T &t);

        /** Returns true if the queue is currently empty. */
        bool empty() const;

        /** When the queue is closed, after it empties all pops will return immediately with a 
            default T value instead of blocking. */
        void close();

    private:
        std::mutex _mutex;
        std::condition_variable _cond;
        std::queue<T> _queue;
        bool _closed {false};
    };
    
}
