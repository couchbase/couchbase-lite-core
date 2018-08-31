//
// Channel.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Channel.hh"
#include "Error.hh"

namespace litecore { namespace actor {

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


    template <class T>
    T Channel<T>::pop(bool &empty, bool wait) {
        std::unique_lock<std::mutex> lock(_mutex);
        while (wait && _queue.empty() && !_closed)
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


    template <class T>
    const T& Channel<T>::front() const {
        std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(_mutex));
        DebugAssert(!_queue.empty());
        return _queue.front();
    }


    template <class T>
    size_t Channel<T>::size() const {
        std::unique_lock<std::mutex> lock(const_cast<std::mutex&>(_mutex));
        return _queue.size();
    }


    template <class T>
    void Channel<T>::close() {
        std::unique_lock<std::mutex> lock(_mutex);
        if (!_closed) {
            _closed = true;
            _cond.notify_all();
        }
    }

} }
