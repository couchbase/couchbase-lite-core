//
// Housekeeper.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "Record.hh"
#include "Actor.hh"
#include "Timer.hh"

namespace litecore {
    class BackgroundDB;
    class DatabaseImpl;


    class Housekeeper : public actor::Actor {
    public:
        /// Creates a Housekeeper for a Database.
        explicit Housekeeper(DatabaseImpl* NONNULL);

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
        void _scheduleExpiration();
        void _doExpiration();

        BackgroundDB* _bgdb;
        actor::Timer _expiryTimer;
    };



}
