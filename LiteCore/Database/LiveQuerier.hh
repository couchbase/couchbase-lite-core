//
// LiveQuerier.hh
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
#include "c4Base.h"
#include "Actor.hh"
#include "fleece/InstanceCounted.hh"
#include "BackgroundDB.hh"
#include "Query.hh"
#include "Logging.hh"
#include <atomic>
#include <chrono>
#include <memory>

namespace litecore {
    class DatabaseImpl;


    /** Runs a query in the background, and optionally watches for the query results to change
        as documents change. */
    class LiveQuerier final : public actor::Actor,
                              BackgroundDB::TransactionObserver,
                              fleece::InstanceCounted
    {
    public:

        class Delegate {
        public:
            virtual void liveQuerierUpdated(QueryEnumerator*, C4Error) =0;
            virtual void liveQuerierStopped() = 0;
            virtual ~Delegate() =default;
        };

        LiveQuerier(DatabaseImpl* NONNULL,
                    Query* NONNULL,
                    bool continuous,
                    Delegate* NONNULL);

        void start(const Query::Options &options);
        
        /** Change the query options of the current running live query. The function will
            discard the current results and re-run the query. If the live query is being stopped or
            stopped, the function will be no-ops. */
        void changeOptions(const Query::Options &options);
        
        void stop();
        
        /** The callback for getting the current result. */
        using CurrentResultCallback = std::function<void(QueryEnumerator*, C4Error)>;
        
        /** Get current result asynchronously. The current result including enumerators and error
            will be reported using the same queue as when the new update is reported to the delegate.
            NOTE: If there has been no query result yet, a NULL enumerator and an empty error will
            be reported. */
        void getCurrentResult(CurrentResultCallback callback);

    protected:
        virtual ~LiveQuerier();
        virtual std::string loggingIdentifier() const override;

    private:
        using clock = std::chrono::steady_clock;

        // TransactionObserver method:
        virtual void transactionCommitted() override;

        void _runQuery(Query::Options);
        void _changeOptions(Query::Options);
        void _stop();
        void _dbChanged(clock::time_point);
        void _currentResult(CurrentResultCallback callback);

        Retained<DatabaseImpl> _database;               // The database
        BackgroundDB* _backgroundDB;                    // Shadow DB on background thread
        Delegate* _delegate;                            // Whom ya gonna call?
        alloc_slice _expression;                        // The query text
        QueryLanguage _language;                        // The query language (JSON or N1QL)
        Retained<Query> _query;                         // Compiled query
        Retained<QueryEnumerator> _currentEnumerator;   // Latest query results
        C4Error _currentError;                          // Latest query error;
        clock::time_point _lastTime;                    // Time the query last ran
        bool _continuous;                               // Do I keep running until stopped?
        bool _waitingToRun {false};                     // Is a call to _runQuery scheduled?
        std::atomic<bool> _stopping {false};            // Has stop() been called?
    };

}
