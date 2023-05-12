//
// BackgroundDB.hh
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
#include "DataFile.hh"
#include "access_lock.hh"
#include <mutex>
#include <vector>

namespace litecore {
    class DatabaseImpl;
    class SequenceTracker;

    class BackgroundDB final : private DataFile::Delegate {
      public:
        explicit BackgroundDB(DatabaseImpl*);
        ~BackgroundDB() override;

        void close();

        access_lock<DataFile*>& dataFile() { return _dataFile; }

        using TransactionTask = function_ref<bool(KeyStore&, SequenceTracker*)>;

        void useInTransaction(slice keyStoreName, TransactionTask task);

        class TransactionObserver {
          public:
            virtual ~TransactionObserver() = default;
            /// This method is called on some random thread, and while a BackgroundDB lock is held.
            /// The implementation must not do anything that might acquire a mutex,
            /// nor call back into BackgroundDB.
            virtual void transactionCommitted() = 0;
        };

        void addTransactionObserver(TransactionObserver* NONNULL);
        void removeTransactionObserver(TransactionObserver* NONNULL);

      private:
        [[nodiscard]] string      databaseName() const override;
        alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        void        externalTransactionCommitted(const SequenceTracker& sourceTracker) override;
        void        notifyTransactionObservers();

        DatabaseImpl*                     _database;
        access_lock<DataFile*>            _dataFile;
        std::vector<TransactionObserver*> _transactionObservers;
        std::mutex                        _transactionObserversMutex;
    };

}  // namespace litecore
