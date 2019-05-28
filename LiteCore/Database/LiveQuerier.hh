//
// LiveQuerier.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Base.h"
#include "Actor.hh"
#include "Query.hh"
#include "Logging.hh"
#include <memory>

namespace c4Internal {
    class Database;
}

namespace litecore {
    class BackgroundDB;
    class DatabaseChangeNotifier;


    /** Runs a query in the background, and optionally watches for the query results to change
        as documents change. */
    class LiveQuerier : public actor::Actor, Logging {
    public:

        class Delegate {
        public:
            virtual void liveQuerierUpdated(QueryEnumerator*, C4Error) =0;
            virtual ~Delegate() =default;
        };

        LiveQuerier(c4Internal::Database *db, Query *query, bool continuous, Delegate*);

        void run(Query::Options options);

        void stop()                             {enqueue(&LiveQuerier::_stop); waitTillCaughtUp();}

    protected:
        virtual ~LiveQuerier();

    private:
        void dbChanged(DatabaseChangeNotifier&) {enqueue(&LiveQuerier::_dbChanged);}

        void _run(Query::Options);
        void _stop();
        void _dbChanged();

        using clock = std::chrono::steady_clock;

        Retained<c4Internal::Database> _database;
        BackgroundDB* _backgroundDB;
        Delegate* _delegate;
        alloc_slice _expression;
        QueryLanguage _language;
        Retained<Query> _query;
        bool _continuous;
        Retained<QueryEnumerator> _currentEnumerator;
        std::unique_ptr<DatabaseChangeNotifier> _dbNotifier;
        clock::time_point _lastTime;
    };

}
