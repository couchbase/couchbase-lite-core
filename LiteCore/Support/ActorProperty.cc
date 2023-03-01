//
// ActorProperty.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ActorProperty.hh"

using namespace std;

namespace litecore { namespace actor {

    template <class T>
    PropertyImpl<T>& PropertyImpl<T>::operator=(const T& t) {
        if ( t != _value ) {
            _value = t;
            for ( auto& observer : _observers ) { observer(_value); }
        }
        return *this;
    }

    template <class T>
    void PropertyImpl<T>::addObserver(Observer<T>& observer) {
        _observers.push_back(observer);
    }

}}  // namespace litecore::actor
