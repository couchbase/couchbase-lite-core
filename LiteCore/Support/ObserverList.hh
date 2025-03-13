//
// ObserverList.hh
//
// Copyright © 2025 Couchbase. All rights reserved.
//

#pragma once
#include "c4ExceptionUtils.hh"
#include "Error.hh"
#include "SmallVector.hh"
#include "fleece/PlatformCompat.hh"  // for ssize_t
#include <algorithm>
#include <mutex>
#include <vector>

namespace litecore {

    /** A thread-safe collection meant for use in implementing the Observer or Publish/Subscribe pattern.

        Its key feature is that _it is safe to mutate the collection during iteration_ -- a situation that commonly
        occurs when a publisher is notifying its subscribers, and one of the subscribers calls back into
        the publisher to remove itself.

        In addition, it is guaranteed that _once an item has been removed, it will not be found by an iterator_.
        (This is a problem with the easy approach of copying the subscriber list before iterating: another thread
        might remove a subscriber and delete it, and then the iterator can call into a deleted object. Even if the
        subscriber isn't deleted, it may not expect to be called anymore which could lead to undefined behavior.) */
    template <class T>
    class ObserverList {
      public:
        /// Adds an item.
        /// @param item  The value to add.
        /// @param unique  If true (the default), will not add a duplicate item.
        /// @returns  True if added, false if it's a duplicate.
        bool add(T item, bool unique = true) {
            std::unique_lock lock(_mutex);
            if ( unique && std::ranges::find(_observers, item) != _observers.end() ) return false;
            _observers.emplace_back(std::move(item));
            return true;
        }

        /// Removes an item. When this method returns, the item is guaranteed not to be returned by any \ref iterate
        /// methods on any threads, meaning that it's safe to delete/invalidate the item.
        /// @param item  The value to remove.
        /// @returns  True if removed, false if not found.
        bool remove(T const& item) {
            std::unique_lock lock(_mutex);
            if ( auto i = std::ranges::find(_observers, item); i != _observers.end() ) {
                if ( i - _observers.begin() < _curIndex ) --_curIndex;  // Fix iterator if items shift underneath it
                _observers.erase(i);
                return true;
            } else {
                return false;
            }
        }

        size_t size() const {
            std::unique_lock lock(_mutex);
            return _observers.size();
        }

        /// Invokes the callback once for each item, passing it a reference.
        /// - Ordering is undefined.
        /// - Exceptions thrown during a callback are caught and logged; they do not stop the iteration.
        /// - It is safe for \ref add or \ref remove to be called by the callback (or something it calls.)
        /// - Items added during a callback will not be returned during this iteration.
        /// - Items removed during a callback will be skipped if they have not yet been returned.
        /// @note  Only one thread can iterate at a time; concurrent calls on other threads will block.
        /// @warning  Reentrant iterations are not allowed, i.e. a callback cannot call \ref iterate.
        template <typename Callback>
        void iterate(Callback const& cb) const noexcept {
            std::unique_lock lock(_mutex);
            Assert(_curIndex == -1, "Illegal reentrant iteration of ObserverList");
            // Iterate backwards so I won't run into items added during a callback.
            for ( _curIndex = ssize_t(_observers.size()) - 1; _curIndex >= 0; --_curIndex ) {
                try {
                    cb(_observers[_curIndex]);
                }
                catchAndWarn()
            }
            // Note: Reentrant iteration could be made legal with a bit more work.
            // I would probably do it by replacing `_curIndex` with a linked list: `iterate` would create a local
            // variable containing {curIndex, prevLink} and point the list head to that. `remove` walks the list.
        }

        /// Calls a method of each observer, using the `iterate` method.
        /// For example, if `observers` is an `ObserverList<Obs*>` and class `Obs` has a method
        /// `changed(int)`, then you could call `observers.notify(&Obs::changed, 42)`.
        template <class U, typename... Args>
        void notify(void (U::*method)(Args...), Args... args) const {
            iterate([&](T const& observer) {
                (observer->*method)(args...);
            });
        }

      private:
        mutable std::recursive_mutex _mutex;          // Allows reentrant calls to add/remove during iteration
        fleece::smallVector<T, 4>    _observers;      // The observer list
        mutable ssize_t              _curIndex = -1;  // Current iteration index, else -1
    };

}  // namespace litecore
