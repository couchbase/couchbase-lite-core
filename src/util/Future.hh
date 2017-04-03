//
//  Future.hh
//  blip_cpp
//
//  Created by Jens Alfke on 1/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include <assert.h>
#include <functional>
#include <thread>
#include <mutex>


namespace litecore {


    /** A simple non-blocking "future": a wrapper for an asynchronously provided value.
        The future starts out empty. The consumer calls onReady() to register a callback when the
        value becomes available. The producer calls fulfil() to provide the value, triggering the
        callback. */
    template <typename T>
    class Future : public RefCounted {
    public:
        Future()
        { }

        void fulfil(const T &value) {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                assert(!_ready);
                _value= value;
                _ready = true;
                if (_callback == nullptr)
                    return; // callback isn't set yet
            }
            // Call the callback [outside of the lock_guard]
            notify();
        }

        typedef std::function<void(T)> Callback;

        void onReady(Callback callback) {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                assert(!_callback);
                _callback = callback;
                if (!_ready)
                    return;
            }
            // Value is already ready; call the callback now [outside of the lock_guard]
            notify();
        }

    private:
        Future(const Future&) =delete;
        Future& operator= (const Future&) =delete;

        void notify() {
            _callback(_value);
            _callback = nullptr;
        }

        std::mutex _mutex;
        bool _ready {false};
        T _value {};
        Callback _callback;
    };

}
