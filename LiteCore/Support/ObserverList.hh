//
// ObserverList.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Compat.h"
#include "SmallVector.hh"
#include "fleece/function_ref.hh"
#include "fleece/PlatformCompat.hh"  // for ssize_t
#include <atomic>
#include <concepts>
#include <mutex>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    class ObserverListBase;

    /** Base class of observer interfaces used by ObserverList<>. */
    class Observer {
      public:
        Observer() = default;

        Observer(const Observer&) {}

      protected:
        virtual ~Observer();

      private:
        friend class ObserverListBase;
        Observer& operator=(const Observer&) = delete;
        Observer(Observer&&)                 = delete;

        std::atomic<ObserverListBase*> _list = nullptr;  // The ObserverList I belong to
    };

    // Abstract superclass of ObserverList<>.
    class ObserverListBase {
      public:
        ObserverListBase() = default;
        ~ObserverListBase();
        void   add(Observer* obs);
        bool   remove(Observer* obs);
        size_t size() const;
        void   iterate(fleece::function_ref<void(Observer*)> const& cb) const noexcept;

      private:
        friend class Observer;
        ObserverListBase(ObserverListBase const&) = delete;
        ObserverListBase(ObserverListBase&&)      = delete;

        mutable std::recursive_mutex      _mutex;          // Allows reentrant calls during iteration
        fleece::smallVector<Observer*, 4> _observers;      // My current Observers
        mutable ssize_t                   _curIndex = -1;  // Current iteration index, else -1
    };

    /** A thread-safe collection for use in implementing the Observer or Publish/Subscribe pattern.
        Its items, "observers", are pointers to instances of `OBS`, a subclass of `Observer`.
        The `notify` method calls a method of all current observers, passing the same arguments to each.

        Its key feature is that _it is safe to add/remove observers during iteration_ -- a situation that commonly
        occurs when an observer is called and during the callback decides to unsubscribe itself.

        In addition, it is guaranteed that _once an observer has been removed, it will not be found by an iterator_.
        (This is a problem with the easy approach of copying the subscriber list before iterating: another thread
        might remove a subscriber and delete it, and then the iterator can call into a deleted object. Even if the
        subscriber isn't deleted, it may not expect to be called anymore which could lead to undefined behavior.)

        Observers automatically remove themselves when they're destructed, so the list never contains
        dangling pointers. */
    template <std::derived_from<Observer> OBS>
    class ObserverList : private ObserverListBase {
      public:
        /// Adds an observer. It must not have been added already.
        /// @param obs  The Observer to add.
        void add(OBS* C4NONNULL obs) { ObserverListBase::add(obs); }

        /// Removes an Observer. After this method returns, the Observer is guaranteed not to be
        /// returned by any \ref iterate methods on any threads, meaning that it's safe to
        /// delete/invalidate the Observer.
        /// @param item  The value to remove.
        /// @returns  True if removed, false if not found.
        bool remove(OBS* C4NONNULL item) { return ObserverListBase::remove(item); }

        /// The number of Observers in the list.
        size_t size() const { return ObserverListBase::size(); }

        /// Invokes the callback once for each Observer, passing it a pointer.
        /// - Ordering is undefined.
        /// - Exceptions thrown during a callback are caught and logged; they do not stop the iteration.
        /// - It is safe for \ref add or \ref remove to be called by the callback (or something it calls.)
        /// - Items added during a callback will not be returned during this iteration.
        /// - Items removed during a callback will be skipped if they have not yet been returned.
        /// @note  Only one thread can iterate at a time; concurrent calls on other threads will block.
        /// @warning  Reentrant iterations are not allowed, i.e. a callback cannot call \ref iterate.
        template <typename Callback>
        void iterate(Callback const& cb) const noexcept {
            ObserverListBase::iterate([&cb](Observer* item) { cb(static_cast<OBS*>(item)); });
        }

        /// Calls a method of each observer, using the `iterate` method.
        /// For example, if `observers` is an `ObserverList<Obs>` and class `Obs` has a method
        /// `changed(int)`, then you could call `observers.notify(&Obs::changed, 42)`.
        template <typename... Params, typename... Args>
        void notify(void (OBS::*method)(Params...), Args... args) const noexcept {
            iterate([&](OBS* observer) { (observer->*method)(args...); });
        }
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
