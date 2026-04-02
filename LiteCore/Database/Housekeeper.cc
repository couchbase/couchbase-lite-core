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
#include "DatabaseImpl.hh"
#include "SequenceTracker.hh"
#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "Logging.hh"
#include "SQLiteKeyStore.hh"
#include "StringUtil.hh"

namespace litecore {
    using namespace actor;
    using namespace std;

    Housekeeper::Housekeeper(C4Collection* coll)
        : Actor(DBLog, stringprintf("Housekeeper for %s", asInternal(coll)->fullName().c_str()))
        , _keyStoreName(asInternal(coll)->keyStore().name())
        , _expiryTimer(std::make_unique<actor::Timer>(std::bind(&Housekeeper::doExpirationAsync, this)))
        , _collection(coll) {}

    void Housekeeper::startExpiration() {
        logInfo("Housekeeper: started expiry.");
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_scheduleExpiration), true);
    }

    void Housekeeper::startMigration() {
        logInfo("Housekeeper: started migrate.");
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_doMigration));
    }

    void Housekeeper::stop() {
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_stop));
        waitTillCaughtUp();
    }

    void Housekeeper::documentExpirationChanged(expiration_t exp) {
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_documentExpirationChanged), exp);
    }

    void Housekeeper::_stop() {
        _expiryTimer = nullptr;
        logVerbose("Housekeeper: stopped.");
    }

    void Housekeeper::_scheduleExpiration(bool onlyIfEarlier) {
        if ( _isStopped() ) return;

        // CBL-3626: Opening the background database synchronously will
        // cause a deadlock when setting document expiration inside of
        // a transaction (inBatch) if it is the first time that document
        // expiration is set.  Opening the background database requires
        // an exclusive transaction.  I wanted to do this in the constructor
        // but calling enqueue there appears to corrupt the whole object, giving
        // it a garbage ref count
        if ( !initBackgroundDB() ) return;

        auto nextExp = _bgdb->dataFile().useLocked<expiration_t>([&](DataFile* df) {
            if ( !df ) { return expiration_t::None; }

            auto& store = df->getKeyStore(_keyStoreName);
            return store.nextExpiration();
        });

        if ( nextExp == expiration_t::None ) {
            logVerbose("Housekeeper: no scheduled document expiration");
            return;
        } else if ( auto delay = nextExp - KeyStore::now(); delay > 0 ) {
            logVerbose("Housekeeper: scheduling expiration in %" PRIi64 "ms", delay);

            // CBL-2392: Since start enqueues an async call to these method, and
            // documentExpirationChanged calls it synchronously there is a race.
            // The race is solved by using fireEarlierAfter, but any further calls
            // should continue to use fireAfter or the timer will never be rescheduled.
            if ( onlyIfEarlier ) {
                _expiryTimer->fireEarlierAfter(chrono::milliseconds(delay));
            } else {
                _expiryTimer->fireAfter(chrono::milliseconds(delay));
            }
        } else {
            _doExpiration();
        }
    }

    void Housekeeper::doExpirationAsync() {
        logInfo("Housekeeper: enqueue _doExpiration");
        enqueue(FUNCTION_TO_QUEUE(Housekeeper::_doExpiration));
    }

    void Housekeeper::_doExpiration() {
        if ( _isStopped() ) return;

        logInfo("Housekeeper: expiring documents...");
        _bgdb->useInTransaction(_keyStoreName, [&](KeyStore& keyStore, SequenceTracker* sequenceTracker) -> bool {
            if ( sequenceTracker ) {
                keyStore.expireRecords([&](slice docID) { sequenceTracker->documentPurged(docID); });
            } else {
                keyStore.expireRecords();
            }
            return true;
        });

        _scheduleExpiration(false);
    }

    void Housekeeper::_documentExpirationChanged(expiration_t exp) {
        if ( _isStopped() ) return;

        if ( exp == expiration_t::None ) return;
        auto delay = exp - KeyStore::now();
        if ( _expiryTimer->fireEarlierAfter(chrono::milliseconds(delay)) )
            logVerbose("Housekeeper: rescheduled expiration, now in %" PRIi64 "ms", delay);
    }

    // Following two constants are subject to performance adjustment
    static unsigned                       sMigrateBatchSize = 10000;
    static constexpr chrono::milliseconds kBatchIntervalMS{100};

    unsigned Housekeeper::setMigrateBatchSize(unsigned batchSize) {
        auto ret          = sMigrateBatchSize;
        sMigrateBatchSize = batchSize;
        return ret;
    }

    void Housekeeper::_doMigration() {
        if ( _isStopped() ) return;
        if ( !initBackgroundDB() ) return;

        int64_t nextMaxRowid = _bgdb->dataFile().useLocked<int64_t>([this](DataFile* dataFile) -> int64_t {
            // return -1 if error.
            if ( !dataFile ) {
                warn("dataFile is NULL in _scheduleMigrate");
                return -1;
            }

            uint64_t             rowidLow = 1;
            ExclusiveTransaction t(dataFile);
            try {
                auto&           infoStore   = dataFile->getKeyStore(DataFile::kInfoKeyStoreName, KeyStore::noSequences);
                Record          rec         = infoStore.get(DataFile::kMaxRowidWithDeletedInDefault);
                uint64_t        rowidHigh   = 0;
                SQLiteKeyStore* sqlKeyStore = SQLiteDataFile::asSQLiteKeyStore(&dataFile->getKeyStore(_keyStoreName));
                Assert(sqlKeyStore);
                if ( !rec.exists() ) {
                    rowidHigh = sqlKeyStore->maxRowid();
                    Record putRec{DataFile::kMaxRowidWithDeletedInDefault};
                    putRec.setBodyAsUInt(rowidHigh);
                    infoStore.setKV(putRec, t);
                } else {
                    rowidHigh = rec.bodyAsUInt();
                }

                // Invariant: rowidHigh == 0 || _migrateTimer != nullptr
                if ( rowidHigh == 0 ) {
                    logInfo("All deleted docs are migrated to deleted table");
                    return 0;
                }
                const unsigned kBatch = sMigrateBatchSize;
                rowidLow              = (rowidHigh >= kBatch) ? rowidHigh - kBatch + 1 : 1;

                logInfo("Migrate deleted docs. Starts from %" PRIu64 " down.", rowidHigh);
                sqlKeyStore->migrateDeletedDocs(_keyStoreName, rowidLow, rowidHigh);
                // Update MaxRowid
                Record putRec(DataFile::kMaxRowidWithDeletedInDefault);
                putRec.setBodyAsUInt(rowidLow - 1);
                infoStore.setKV(putRec, t);
            } catch ( const exception& exc ) {
                warn("Migration of deleted docs hit an exception %s", exc.what());
                t.abort();
                return -1;
            }
            t.commit();

            // Invariant: rowidLow >= 1
            return (int64_t)(rowidLow - 1);
        });

        if ( nextMaxRowid < 0 ) return;  // Error: already logged in the above catch.

        if ( nextMaxRowid > 0 ) enqueueAfter(kBatchIntervalMS, FUNCTION_TO_QUEUE(Housekeeper::_doMigration));
        logInfo("Migrate deleted docs. Next rowid to start from %" PRId64 " down.", nextMaxRowid);
    }

    bool Housekeeper::initBackgroundDB() {
        if ( !_bgdb && _collection && _collection->isValid() ) {
            _bgdb       = asInternal(_collection->getDatabase())->backgroundDatabase();
            _collection = nullptr;  // No longer needed, release the retain
            logInfo("Housekeeper: opening background database for the Housekeeper...");
        }
        if ( !_bgdb ) {
            logError("Housekeeping unable to start, collection is closed and/or deleted!");
            return false;
        } else {
            return true;
        }
    }

}  // namespace litecore
