//
//  ActorProperty.cc
//  blip_cpp
//
//  Created by Jens Alfke on 3/3/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ActorProperty.hh"

using namespace std;

namespace litecore { namespace actor {

    template <class T>
    PropertyImpl<T>& PropertyImpl<T>::operator= (const T &t) {
        if (t != _value) {
            _value = t;
            for (auto &observer : _observers) {
                observer(_value);
            }
        }
        return *this;
    }


    template <class T>
    void PropertyImpl<T>::addObserver(Observer<T> &observer) {
        _observers.push_back(observer);
    }

} }
