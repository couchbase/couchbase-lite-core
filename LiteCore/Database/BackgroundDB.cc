//
// BackgroundDB.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "BackgroundDB.hh"
#include "c4Internal.hh"
#include "DatabaseImpl.hh"
#include "DataFile.hh"
#include "SequenceTracker.hh"

namespace litecore {
    using namespace std::placeholders;
    using namespace std;

    BackgroundDB::BackgroundDB(DatabaseImpl* db) : _database(db) {
        _dataFile.useLocked([db, this](DataFile*& df) {
            // CBL-2543: Don't actually call openAnother until inside the constructor
            // otherwise, openAnother could quickly call back into externalTransactionCommitted
            // before this BackgroundDB is fully initialized.
            df = db->dataFile()->openAnother(this);
            df->setDatabaseTag(kDatabaseTag_BackgroundDB);
        });
    }

    void BackgroundDB::close() {
        _dataFile.useLocked([this](DataFile*& df) {
            delete df;
            df = nullptr;
        });
    }

    BackgroundDB::~BackgroundDB() { close(); }

    string BackgroundDB::databaseName() const { return _database->databaseName(); }

    alloc_slice BackgroundDB::blobAccessor(const fleece::impl::Dict* dict) const {
        return _database->blobAccessor(dict);
    }

    void BackgroundDB::externalTransactionCommitted(const SequenceTracker& sourceTracker) {
        notifyTransactionObservers();
    }

    void BackgroundDB::useInTransaction(slice keyStoreName, TransactionTask task) {
        _dataFile.useLocked([this, keyStoreName, task](DataFile* dataFile) {
            if ( !dataFile ) return;
            ExclusiveTransaction t(dataFile);
            KeyStore&            keyStore = dataFile->getKeyStore(keyStoreName);
            SequenceTracker      sequenceTracker(keyStoreName);
            sequenceTracker.beginTransaction();

            bool commit;
            try {
                commit = task(keyStore, &sequenceTracker);
            } catch ( const exception& ) {
                t.abort();
                sequenceTracker.endTransaction(false);
                throw;
            }

            if ( !commit ) {
                t.abort();
                sequenceTracker.endTransaction(false);
                return;
            }

            t.commit();
            // Notify other Database instances of any changes:
            t.notifyCommitted(sequenceTracker);
            sequenceTracker.endTransaction(true);
            // Notify my own observers:
            notifyTransactionObservers();
        });
    }

    void BackgroundDB::addTransactionObserver(TransactionObserver* obs) {
        LOCK(_transactionObserversMutex);
        _transactionObservers.push_back(obs);
    }

    void BackgroundDB::removeTransactionObserver(TransactionObserver* obs) {
        LOCK(_transactionObserversMutex);
        auto i = std::find(_transactionObservers.begin(), _transactionObservers.end(), obs);
        if ( i != _transactionObservers.end() ) _transactionObservers.erase(i);
    }

    void BackgroundDB::notifyTransactionObservers() {
        LOCK(_transactionObserversMutex);
        for ( auto obs : _transactionObservers ) obs->transactionCommitted();
    }

}  // namespace litecore
