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

namespace litecore { namespace actor {

    /** A simple thread-safe producer/consumer queue. */
    template <class T>
    class Channel {
    public:

        /** Pushes a new value to the front of the queue.
            @return  True if the queue was empty before the push. */
        bool push(const T &t);

        /** Pops the next value from the end of the queue.
            If the queue is empty, blocks until another thread adds something to the queue.
            If the queue is closed and empty, returns a default (zero) T.
            @param empty  Will be set to true if the queue is now empty. */
        T pop(bool &empty)               {return pop(empty, true);}

        /** Pops the next value from the end of the queue.
            If the queue is empty, immediately returns a default (zero) T.
            @param empty  Will be set to true if the queue is now empty. */
        T popNoWaiting(bool &empty)      {return pop(empty, false);}

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
        std::mutex _mutex;
        
    private:
        T pop(bool &empty, bool wait);

        std::condition_variable _cond;
        std::queue<T> _queue;
        bool _closed {false};
    };
    
} }
