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

        using Task = std::function<void(DataFile*)>;
        using TransactionTask = std::function<bool(DataFile*, SequenceTracker*)>;

        void doTask(Task task)              {enqueue(&BackgroundDB::_doTask, task);}
        void inTransactionDo(TransactionTask task)
                                            {enqueue(&BackgroundDB::_inTransactionDo, task);}

    private:
        slice fleeceAccessor(slice recordBody) const override;
        alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        void _close();
        void _doTask(Task);
        void _inTransactionDo(TransactionTask);

        Retained<c4Internal::Database> _database;
        std::unique_ptr<DataFile> _bgDataFile;
    };


    class BackgroundQuerier {
    public:
        BackgroundQuerier(c4Internal::Database *db, Query *query);

        using Callback = std::function<void(Retained<QueryEnumerator>, error)>;

        void run(Query::Options, Callback);
        void refresh(QueryEnumerator*, Callback);

    private:
        void run(Query::Options, Retained<QueryEnumerator>, Callback);

        Retained<BackgroundDB> _backgroundDB;
        alloc_slice _expression;
        QueryLanguage _language;
        Retained<Query> _query;
    };

}
