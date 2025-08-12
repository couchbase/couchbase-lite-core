//
// Observer.hh
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
#include <atomic>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    class ObserverListBase;

    /** Base class of observer interfaces used by ObserverList<>. */
    class Observer {
      public:
        Observer() = default;

        Observer(const Observer&) {}  // deliberately avoids setting _list

        /// Removes this observer from any ObserverList it was added to.
        /// @warning Any subclass that implements observer methods **must** call this (from its
        /// destructor or earlier) if has been added to an ObserverList. Otherwise its notification
        /// methods could be called after it's been destructed, causing crashes or worse.
        void removeFromObserverList();

      protected:
        virtual ~Observer();

      private:
        friend class ObserverListBase;
        Observer& operator=(const Observer&) = delete;
        Observer(Observer&&)                 = delete;

        std::atomic<ObserverListBase*> _list = nullptr;  // The ObserverList I belong to
    };
}  // namespace litecore

C4_ASSUME_NONNULL_END
