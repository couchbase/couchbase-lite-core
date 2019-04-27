//
//  access_lock.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include <mutex>


namespace litecore {

    template <class T>
    class access_lock {
    public:
        access_lock(T &&contents)
        :_contents(std::move(contents))
        { }

        template <class LAMBDA>
        void use(LAMBDA callback) const {
            std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(_mutex));
            callback(_contents);
        }

        template <class RESULT, class LAMBDA>
        RESULT use(LAMBDA callback) const {
            std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(_mutex));
            return callback(_contents);
        }

    protected:
        // Internal version of use() that's non-const, so callback is allowed to change _contents.
        template <class LAMBDA>
        void useAndSet(LAMBDA callback) {
            std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex&>(_mutex));
            callback(_contents);
        }

    private:
        T _contents;
        std::recursive_mutex _mutex;
    };

}
