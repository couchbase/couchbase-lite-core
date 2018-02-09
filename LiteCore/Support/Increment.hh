//
// Increment.hh
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

#pragma once
#include "Error.hh"


namespace litecore {

    template <class T>
    static T increment(T &value, T by =1) {
        Assert(value + by >= value, "overflow incrementing a counter");
        value += by;
        return value;
    }

    template <class T>
    static T decrement(T &value, T by =1) {
        Assert(value >= by, "underflow decrementing a counter");
        value -= by;
        return value;
    }

    template <class T>
    class temporary_increment {
    public:
        temporary_increment(T &value, T by =1)
        :_value(value)
        ,_by(by)
        {
            increment(value, by);
        }

        temporary_increment(const temporary_increment &&other)
        :temporary_increment(other._value, other._by)
        {
            other._by = 0;
        }

        void end() {
            decrement(_value, _by);
            _by = 0;
        }

        ~temporary_increment() {
            decrement(_value, _by);
        }

    private:
        temporary_increment(const temporary_increment&) =delete;
        temporary_increment& operator=(const temporary_increment&) =delete;

        T &_value;
        T _by;
    };

}
