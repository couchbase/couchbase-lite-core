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
#include "CollectionImpl.hh"
#include "c4Internal.hh"
#include "DatabaseImpl.hh"
#include "SequenceTracker.hh"
#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <inttypes.h>

namespace litecore {
    using namespace actor;
    using namespace std;

    Housekeeper::Housekeeper(C4Collection *coll)
    :Actor(DBLog, format("Housekeeper for %.*s", SPLAT(coll->getName())))
    ,_keyStoreName(asInternal(coll)->keyStore().name())
    ,_bgdb(asInternal(coll->getDatabase())->backgroundDatabase())
    ,_expiryTimer(std::bind(&Housekeeper::_doExpiration, this))
    { }


    void Housekeeper::start() {
        logInfo("Housekeeper: started.");
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_scheduleExpiration));
    }


    void Housekeeper::stop() {
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_stop));
        waitTillCaughtUp();
    }


    void Housekeeper::_stop() {
        _expiryTimer.stop();
        logVerbose("Housekeeper: stopped.");
    }


    void Housekeeper::_scheduleExpiration() {
        expiration_t nextExp = _bgdb->dataFile().useLocked<expiration_t>([&](DataFile *df) {
            return df ? df->defaultKeyStore().nextExpiration() : 0;
        });
        if (nextExp == 0) {
            logVerbose("Housekeeper: no scheduled document expiration");
            return;
        } else if (expiration_t delay = nextExp - KeyStore::now(); delay > 0) {
            logVerbose("Housekeeper: scheduling expiration in %" PRIi64 "ms", delay);
            _expiryTimer.fireAfter(chrono::milliseconds(delay));
        } else {
            _doExpiration();
        }
    }


    void Housekeeper::_doExpiration() {
        logVerbose("Housekeeper: expiring documents...");
        _bgdb->useInTransaction(DataFile::kDefaultKeyStoreName,
                                [&](KeyStore &keyStore, SequenceTracker *sequenceTracker) -> bool {
            if (sequenceTracker) {
                keyStore.expireRecords([&](slice docID) {
                    sequenceTracker->documentPurged(docID);
                });
            } else {
                keyStore.expireRecords();
            }
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
            logVerbose("Housekeeper: rescheduled expiration, now in %" PRIi64 "ms", delay);
    }

}
