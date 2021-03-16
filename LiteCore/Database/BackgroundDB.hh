//
// BackgroundDB.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Actor.hh"
#include "DataFile.hh"
#include "access_lock.hh"
#include "function_ref.hh"
#include <mutex>
#include <vector>

namespace litecore {
    class DatabaseImpl;
    class SequenceTracker;


    class BackgroundDB final : public access_lock<DataFile*>, private DataFile::Delegate {
    public:
        BackgroundDB(DatabaseImpl*);
        ~BackgroundDB();

        void close();

        using TransactionTask = function_ref<bool(DataFile*, SequenceTracker*)>;

        void useInTransaction(TransactionTask task);

        class TransactionObserver {
        public:
            virtual ~TransactionObserver() =default;
            /// This method is called on some random thread, and while a BackgroundDB lock is held.
            /// The implementation must not do anything that might acquire a mutex,
            /// nor call back into BackgroundDB.
            virtual void transactionCommitted() =0;
        };

        void addTransactionObserver(TransactionObserver* NONNULL);
        void removeTransactionObserver(TransactionObserver* NONNULL);

    private:
        alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        void externalTransactionCommitted(const SequenceTracker &sourceTracker) override;
        void notifyTransactionObservers();

        DatabaseImpl* _database;
        std::vector<TransactionObserver*> _transactionObservers;
        std::mutex _transactionObserversMutex;
    };

}
