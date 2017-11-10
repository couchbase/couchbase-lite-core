//
//  Increment.hh
//  LiteCore
//
//  Created by Jens Alfke on 11/10/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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
