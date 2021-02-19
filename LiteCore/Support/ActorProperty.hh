//
// ActorProperty.hh
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
#include "Actor.hh"
#include <functional>
#include <vector>

namespace litecore { namespace actor {

    template <class T>
    class Observer;

    /** Implementation of an Actor property. This would be a private member variable of an Actor. */
    template <class T>
    class PropertyImpl {
    public:
        explicit PropertyImpl(Actor *owner)     :_owner(*owner) { }
        explicit PropertyImpl(Actor *owner, T t):_owner(*owner), _value(t) { }

        T get() const                           {return _value;}
        operator T() const                      {return _value;}
        PropertyImpl& operator= (const T &t);

        void addObserver(Observer<T> &observer);

    private:
        Actor &_owner;
        T _value {};
        std::vector<Observer<T>> _observers;
    };


    template <class T>
    class ObservedProperty {
    public:
        ~ObservedProperty();

        T get() const                           {return _value;}
        operator T() const                      {return _value;}
    private:
        void receiveValue(T t)                  {_value = t;}

        Retained<Actor> &_provider;
        T _value;
    };


    /** Public Actor property. This would be a public member variable of an Actor. */
    template <class T>
    class Property {
    public:
        explicit Property(PropertyImpl<T> &prop)     :_impl(prop) { }

        using Observer = std::function<void(T)>;

        void addObserver(Observer &observer)        {_impl.addObserver(observer);}
        void removeObserver(Actor &a);

    private:
        PropertyImpl<T> &_impl;
    };

} }
