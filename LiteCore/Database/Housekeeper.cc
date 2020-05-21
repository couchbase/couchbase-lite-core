//
// Housekeeper.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "Housekeeper.hh"
#include "Database.hh"
#include "SequenceTracker.hh"
#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "Logging.hh"
#include <inttypes.h>

namespace litecore {
    using namespace c4Internal;
    using namespace actor;

    Housekeeper::Housekeeper(Database *db)
    :Actor("Housekeeper")
    ,_bgdb(db->backgroundDatabase())
    ,_expiryTimer(std::bind(&Housekeeper::_doExpiration, this))
    { }


    void Housekeeper::start() {
        enqueue(&Housekeeper::_scheduleExpiration);
    }


    void Housekeeper::stop() {
        enqueue(&Housekeeper::_stop);
        waitTillCaughtUp();
    }


    void Housekeeper::_stop() {
        _expiryTimer.stop();
        LogToAt(DBLog, Verbose, "Housekeeper: stopped.");
    }


    void Housekeeper::_scheduleExpiration() {
        expiration_t nextExp = _bgdb->use<expiration_t>([&](DataFile *df) {
            return df ? df->defaultKeyStore().nextExpiration() : 0;
        });
        if (nextExp == 0) {
            LogToAt(DBLog, Verbose, "Housekeeper: no scheduled document expiration");
            return;
        } else if (expiration_t delay = nextExp - KeyStore::now(); delay > 0) {
            LogToAt(DBLog, Verbose, "Housekeeper: scheduling expiration in %" PRIi64 "ms", delay);
            _expiryTimer.fireAfter(chrono::milliseconds(delay));
        } else {
            _doExpiration();
        }
    }


    void Housekeeper::_doExpiration() {
        LogToAt(DBLog, Verbose, "Housekeeper: expiring documents...");
        _bgdb->useInTransaction([&](DataFile* dataFile, SequenceTracker *sequenceTracker) -> bool {
            KeyStore::ExpirationCallback callback;
            auto &keyStore = dataFile->defaultKeyStore();
            if (sequenceTracker)
                callback = [&](slice docID) { sequenceTracker->documentPurged(docID); };
            keyStore.expireRecords(callback);
            return true;
        });

        _scheduleExpiration();
    }


    void Housekeeper::documentExpirationChanged(expiration_t exp) {
        // This doesn't have to be enqueued, since Timer is thread-safe.
        if (exp == 0)
            return;
        expiration_t delay = exp - KeyStore::now();
        if (_expiryTimer.fireEarlierAfter(chrono::milliseconds(delay)))
            LogToAt(DBLog, Verbose, "Housekeeper: rescheduled expiration, now in %" PRIi64 "ms", delay);
    }

}
