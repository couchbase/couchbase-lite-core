//
// Housekeeper.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "Record.hh"
#include "Actor.hh"
#include "Timer.hh"

struct C4Collection;

namespace litecore {
    class BackgroundDB;

    class Housekeeper : public actor::Actor {
      public:
        /// Creates a Housekeeper for a Collection.
        explicit Housekeeper(C4Collection* NONNULL);

        /// Asynchronously starts the Housekeeper task.
        void start();

        /// Synchronously stops the Housekeeper task. After this returns it will do nothing.
        void stop();

        /// Informs the Housekeeper that a document's expiration time has changed, so it can
        /// reschedule its next expiration for earlier if necessary.
        void documentExpirationChanged(expiration_t exp);

      private:
        void _start();
        void _stop();

        bool _isStopped() const { return !_expiryTimer; }

        void _scheduleExpiration(bool onlyIfEarlier);
        void _doExpiration();
        void _documentExpirationChanged(expiration_t exp);
        void doExpirationAsync();

        alloc_slice                    _keyStoreName;
        BackgroundDB*                  _bgdb{nullptr};
        std::unique_ptr<actor::Timer>  _expiryTimer;
        fleece::Retained<C4Collection> _collection;  // Used for initialization only
    };
}  // namespace litecore
