//
// LiveQuerier.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "LiveQuerier.hh"
#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "Database.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include <inttypes.h>

namespace litecore {
    using namespace actor;
    using namespace std::placeholders;


    // Threshold for rapidity of database changes. If it's been this long since the last change,
    // we re-query after the short delay. Otherwise we use the long delay. This allows for very
    // low latency if changes are not too rapid, while also not flooding the app with notifications
    // if changes are rapid.
    static constexpr delay_t kRapidChanges = chrono::milliseconds(250);

    static constexpr delay_t kShortDelay   = chrono::milliseconds(  0);
    static constexpr delay_t kLongDelay    = chrono::milliseconds(500);


    LiveQuerier::LiveQuerier(c4Internal::Database *db,
                             Query *query,
                             bool continuous,
                             Delegate *delegate)
    :Logging(QueryLog)
    ,_database(db)
    ,_backgroundDB(db->backgroundDatabase())
    ,_expression(query->expression())
    ,_language(query->language())
    ,_continuous(continuous)
    ,_delegate(delegate)
    {
        logInfo("Created on Query %s", query->loggingName().c_str());
        // Note that we don't keep a reference to `_query`, because it's tied to `db`, but we
        // need to run the query on `_backgroundDB`. So instead we save the query text and
        // language, and create a new Query instance the first time `_runQuery` is called.
    }


    LiveQuerier::~LiveQuerier() {
        if (_query)
            _stop();
        logVerbose("Deleted");
    }


    std::string LiveQuerier::loggingIdentifier() const {
        return string(_expression);
    }


    void LiveQuerier::start(Query::Options options) {
        _lastTime = clock::now();
        enqueue(&LiveQuerier::_runQuery, options);
    }


    void LiveQuerier::stop() {
        logInfo("Stopping");
        _stopping = true;
        enqueue(&LiveQuerier::_stop);
    }


    // Database change (transaction committed) notification
    void LiveQuerier::transactionCommitted() {
        enqueue(&LiveQuerier::_dbChanged, clock::now());
    }


#pragma mark - ACTOR METHODS (single-threaded):


    void LiveQuerier::_stop() {
        if (_query) {
            _backgroundDB->use([&](DataFile *df) {
                _query = nullptr;
                _currentEnumerator = nullptr;
                if (_continuous)
                    _backgroundDB->removeTransactionObserver(this);
            });
        }
        logVerbose("...stopped");
        _stopping = false;
    }


    void LiveQuerier::_dbChanged(clock::time_point when) {
        // Do nothing if there's already a _runQuery call pending (but not yet running),
        // or I've already been told to stop, or the query can't be run:
        if (_waitingToRun || _stopping || !_currentEnumerator)
            return;

        delay_t idleTime = when - _lastTime;
        _lastTime = when;

        delay_t delay = (idleTime <= kRapidChanges) ? kLongDelay : kShortDelay;
        logVerbose("DB changed after %.3f sec. Triggering query in %.3f secs",
                   idleTime.count(), delay.count());
        enqueueAfter(delay, &LiveQuerier::_runQuery, _currentEnumerator->options());
        _waitingToRun = true;
    }


    void LiveQuerier::_runQuery(Query::Options options) {
        if (_stopping)
            return;

        _waitingToRun = false;
        logVerbose("Running query...");
        Retained<QueryEnumerator> newQE;
        C4Error error = {};
        fleece::Stopwatch st;
        _backgroundDB->use([&](DataFile *df) {
            try {
                // Create my own Query object associated with the Backgrounder's DataFile:
                if (!_query) {
                    _query = df->defaultKeyStore().compileQuery(_expression, _language);
                    if (_continuous)
                        _backgroundDB->addTransactionObserver(this);
                }
                // Now run the query:
                newQE = _query->createEnumerator(&options);
            } catchError(&error);
        });
        auto time = st.elapsedMS();

        if (!newQE)
            logError("Query failed with error %s", c4error_descriptionStr(error));

        if (_continuous) {
            if (newQE) {
                if (_currentEnumerator && !_currentEnumerator->obsoletedBy(newQE)) {
                    logVerbose("Results unchanged at seq %" PRIu64 " (%.3fms)",
                               newQE->lastSequence(), time);
                    return; // no delegate call
                }
                logInfo("Results changed at seq %" PRIu64 " (%.3fms)", newQE->lastSequence(), time);
                _currentEnumerator = newQE;
            }
        } else {
            logInfo("...finished one-shot query in %.3fms", time);
        }

        if (_stopping)
            return;
        
        _delegate->liveQuerierUpdated(newQE, error);
    }

}
