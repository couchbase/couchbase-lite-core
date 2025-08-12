//
// ObserverList.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ObserverList.hh"
#include "c4ExceptionUtils.hh"
#include "Error.hh"
#include <algorithm>

namespace litecore {

    Observer::~Observer() { Assert(!_list, "An Observer subclass forgot to call removeObserver()"); }

    void Observer::removeFromObserverList() {
        if ( auto list = _list.load() ) list->remove(this);
    }

    size_t ObserverListBase::size() const {
        std::unique_lock lock(_mutex);
        return _observers.size();
    }

    ObserverListBase::~ObserverListBase() {
        std::unique_lock lock(_mutex);
        Assert(_curIndex == -1, "ObserverList being destructed during iteration");
        for ( auto& obs : _observers ) obs->_list = nullptr;
    }

    void ObserverListBase::add(Observer* obs) {
        std::unique_lock  lock(_mutex);
        ObserverListBase* expected = nullptr;
        // Point observer's `_list` to me, but fail if it's already set:
        if ( !obs->_list.compare_exchange_strong(expected, this) )
            error::_throw(error::InvalidParameter, "Observer already belongs to an ObserverList");
        _observers.emplace_back(obs);
    }

    bool ObserverListBase::remove(Observer* obs) {
        std::unique_lock  lock(_mutex);
        ObserverListBase* expected = this;
        // Clear observer's `_list`, if it pointed to me:
        if ( !obs->_list.compare_exchange_strong(expected, nullptr) ) return false;
        // Remove it from my vector:
        if ( auto i = std::ranges::find(_observers, obs); i != _observers.end() ) {
            if ( i - _observers.begin() < _curIndex ) --_curIndex;  // Fix iterator if items shift underneath it
            _observers.erase(i);
            return true;
        } else {
            return false;
        }
    }

    void ObserverListBase::iterate(fleece::function_ref<void(Observer*)> const& cb) const noexcept {
        std::unique_lock lock(_mutex);
        Assert(_curIndex == -1, "Illegal reentrant iteration of ObserverList");
        // Iterate backwards so I won't run into items added during a callback.
        for ( _curIndex = ssize_t(_observers.size()) - 1; _curIndex >= 0; --_curIndex ) {
            try {
                DebugAssert(_observers[_curIndex]->_list == this);
                cb(_observers[_curIndex]);
            }
            catchAndWarn()
        }
        // Note: Reentrant iteration could be made legal with a bit more work.
        // I would probably do it by replacing `_curIndex` with a linked list: `iterate` would create a local
        // variable containing {curIndex, prevLink} and point the list head to that. `remove` walks the list.
    }

}  // namespace litecore
