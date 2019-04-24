//
// BackgroundDB.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Actor.hh"
#include "DataFile.hh"
#include "Query.hh"
#include <functional>
#include <memory>

namespace c4Internal {
    class Database;
}

namespace litecore {
    class SequenceTracker;


    class BackgroundDB : public actor::Actor, private DataFile::Delegate {
    public:
        BackgroundDB(c4Internal::Database*);

        void close()                        {enqueue(&BackgroundDB::_close); waitTillCaughtUp();}

        using Task = std::function<bool(DataFile*, SequenceTracker*)>;

        void inTransactionDo(Task task)     {enqueue(&BackgroundDB::_inTransactionDo, task);}

        using RefreshQueryCallback = std::function<void(std::shared_ptr<QueryEnumerator>, error)>;

        void runQuery(Query*, Query::Options, RefreshQueryCallback);
        void refreshQuery(QueryEnumerator*, RefreshQueryCallback);

    private:
        slice fleeceAccessor(slice recordBody) const override;
        alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        void _close();
        void _inTransactionDo(Task);
        void refreshQuery(Query *query, Query::Options,
                          QueryEnumerator*, RefreshQueryCallback);
        void _refreshQuery(alloc_slice expression, Query::Options options,
                           RefreshQueryCallback callback);

        Retained<c4Internal::Database> _database;
        std::unique_ptr<DataFile> _bgDataFile;
    };

}
