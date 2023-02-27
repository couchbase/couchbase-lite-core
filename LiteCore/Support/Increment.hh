//
// Increment.hh
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

namespace litecore {

    template <class T>
    static T _increment(T &value, const char *name NONNULL, T by = 1) {
        Assert(value + by >= value, "overflow incrementing %s", name);
        value += by;
        return value;
    }

    template <class T>
    static T _decrement(T &value, const char *name NONNULL, T by = 1) {
        Assert(value >= by, "underflow decrementing %s", name);
        value -= by;
        return value;
    }

    template <class T>
    class temporary_increment {
      public:
        temporary_increment(T &value, T by = 1) : _value(value), _by(by) { _increment(value, by); }

        temporary_increment(const temporary_increment &&other) : temporary_increment(other._value, other._by) {
            other._by = 0;
        }

        void end() {
            _decrement(_value, _by);
            _by = 0;
        }

        ~temporary_increment() { _decrement(_value, _by); }

      private:
        temporary_increment(const temporary_increment &)            = delete;
        temporary_increment &operator=(const temporary_increment &) = delete;

        T &_value;
        T  _by;
    };

#define increment(VAL, ...) litecore::_increment(VAL, #VAL, ##__VA_ARGS__)
#define decrement(VAL, ...) litecore::_decrement(VAL, #VAL, ##__VA_ARGS__)

}  // namespace litecore
