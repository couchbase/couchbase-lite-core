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
#include "InstanceCounted.hh"
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
            virtual ~Delegate() =default;
        };

        LiveQuerier(DatabaseImpl* NONNULL,
                    Query* NONNULL,
                    bool continuous,
                    Delegate* NONNULL);

        void start(const Query::Options &options);

        void stop();

    protected:
        virtual ~LiveQuerier();
        virtual std::string loggingIdentifier() const override;

    private:
        using clock = std::chrono::steady_clock;

        // TransactionObserver method:
        virtual void transactionCommitted() override;

        void _runQuery(Query::Options);
        void _stop();
        void _dbChanged(clock::time_point);

        Retained<DatabaseImpl> _database;               // The database
        BackgroundDB* _backgroundDB;                    // Shadow DB on background thread
        Delegate* _delegate;                            // Whom ya gonna call?
        alloc_slice _expression;                        // The query text
        QueryLanguage _language;                        // The query language (JSON or N1QL)
        Retained<Query> _query;                         // Compiled query
        Retained<QueryEnumerator> _currentEnumerator;   // Latest query results
        clock::time_point _lastTime;                    // Time the query last ran
        bool _continuous;                               // Do I keep running until stopped?
        bool _waitingToRun {false};                     // Is a call to _runQuery scheduled?
        std::atomic<bool> _stopping {false};            // Has stop() been called?
    };

}
