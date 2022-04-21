//
// Housekeeper.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
    :Actor(DBLog, format("Housekeeper for %s", asInternal(coll)->fullName().c_str()))
    ,_keyStoreName(asInternal(coll)->keyStore().name())
    ,_bgdb(asInternal(coll->getDatabase())->backgroundDatabase())
    ,_expiryTimer(std::bind(&Housekeeper::_doExpiration, this))
    { }


    void Housekeeper::start() {
        logInfo("Housekeeper: started.");
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_scheduleExpiration), true);
    }


    void Housekeeper::stop() {
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_stop));
        waitTillCaughtUp();
    }


    void Housekeeper::_stop() {
        _expiryTimer.stop();
        logVerbose("Housekeeper: stopped.");
    }


    void Housekeeper::_scheduleExpiration(bool onlyIfEarlier) {
        expiration_t nextExp = _bgdb->dataFile().useLocked<expiration_t>([&](DataFile *df) {
            return df ? df->defaultKeyStore().nextExpiration() : expiration_t::None;
        });
        if (nextExp == expiration_t::None) {
            logVerbose("Housekeeper: no scheduled document expiration");
            return;
        } else if (auto delay = nextExp - KeyStore::now(); delay > 0) {
            logVerbose("Housekeeper: scheduling expiration in %" PRIi64 "ms", delay);
            
            // CBL-2392: Since start enqueues an async call to these method, and
            // documentExpirationChanged calls it synchronously there is a race.
            // The race is solved by using fireEarlierAfter, but any further calls
            // should continue to use fireAfter or the timer will never be rescheduled.
            if(onlyIfEarlier) {
                _expiryTimer.fireEarlierAfter(chrono::milliseconds(delay));
            } else {
                _expiryTimer.fireAfter(chrono::milliseconds(delay));
            }
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

        _scheduleExpiration(false);
    }


    void Housekeeper::documentExpirationChanged(expiration_t exp) {
        // This doesn't have to be enqueued, since Timer is thread-safe.
        if (exp == expiration_t::None)
            return;
        auto delay = exp - KeyStore::now();
        if (_expiryTimer.fireEarlierAfter(chrono::milliseconds(delay)))
            logVerbose("Housekeeper: rescheduled expiration, now in %" PRIi64 "ms", delay);
    }

}
