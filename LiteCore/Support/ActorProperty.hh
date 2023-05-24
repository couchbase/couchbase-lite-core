//
// ActorProperty.hh
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
#include "Actor.hh"
#include <functional>
#include <vector>

namespace litecore::actor {

    template <class T>
    class Observer;

    /** Implementation of an Actor property. This would be a private member variable of an Actor. */
    template <class T>
    class PropertyImpl {
      public:
        explicit PropertyImpl(Actor* owner) : _owner(*owner) {}

        explicit PropertyImpl(Actor* owner, T t) : _owner(*owner), _value(t) {}

        T get() const { return _value; }

        explicit operator T() const { return _value; }

        PropertyImpl& operator=(const T& t);

        void addObserver(Observer<T>& observer);

      private:
        Actor&                   _owner;
        T                        _value{};
        std::vector<Observer<T>> _observers;
    };

    template <class T>
    class ObservedProperty {
      public:
        ~ObservedProperty();

        T get() const { return _value; }

        explicit operator T() const { return _value; }

      private:
        void receiveValue(T t) { _value = t; }

        Retained<Actor>& _provider;
        T                _value;
    };

    /** Public Actor property. This would be a public member variable of an Actor. */
    template <class T>
    class Property {
      public:
        explicit Property(PropertyImpl<T>& prop) : _impl(prop) {}

        using Observer = std::function<void(T)>;

        void addObserver(Observer& observer) { _impl.addObserver(observer); }

        void removeObserver(Actor& a);

      private:
        PropertyImpl<T>& _impl;
    };

}  // namespace litecore::actor
