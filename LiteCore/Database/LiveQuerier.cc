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
#include "SequenceTracker.hh"
#include "c4ExceptionUtils.hh"
#include <inttypes.h>

namespace litecore {
    using namespace actor;
    using namespace std::placeholders;


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
        logInfo("Created on Query %p", query);
    }


    LiveQuerier::~LiveQuerier() {
        if (_query || _dbNotifier)
            _stop();
    }


    void LiveQuerier::run(Query::Options options) {
        _lastTime = clock::now();
        enqueue(&LiveQuerier::_run, options);
    }


    // Runs the query.
    void LiveQuerier::_run(Query::Options options) {
        logVerbose("Running query...");
        Retained<QueryEnumerator> newQE;
        C4Error error = {};
        fleece::Stopwatch st;
        _backgroundDB->use([&](DataFile *df) {
            try {
                // Create my own Query object associated with the Backgrounder's DataFile:
                if (!_query) {
                    _query = df->defaultKeyStore().compileQuery(_expression, _language);
                    _expression = nullslice;
                }
                // Now run the query:
                newQE = _query->createEnumerator(&options);
            } catchError(&error);
        });
        auto time = st.elapsedMS();

        if (!newQE)
            logError("Query failed with error %s", c4error_descriptionStr(error));

        if (!_continuous) {
            logInfo("...finished one-shot query in %.3fms", time);
            _delegate->liveQuerierUpdated(newQE, error);
            return;
        }

        if (newQE) {
            if (_currentEnumerator && !_currentEnumerator->obsoletedBy(newQE)) {
                logVerbose("Results unchanged at seq %" PRIu64 " (%.3fms)",
                           newQE->lastSequence(), time);
            } else {
                logInfo("Results changed at seq %" PRIu64 " (%.3fms)", newQE->lastSequence(), time);
                _currentEnumerator = newQE;
                _delegate->liveQuerierUpdated(newQE, error);
            }
        }

        sequence_t after = _currentEnumerator ? _currentEnumerator->lastSequence() : 0;
        sequence_t lastSequence = 0;

        _database->sequenceTracker().use([&](SequenceTracker &st) {
            if (_dbNotifier == nullptr) {
                // Start the db change notifier:
                _dbNotifier.reset(new DatabaseChangeNotifier(st,
                                                             bind(&LiveQuerier::dbChanged, this, _1),
                                                             after));
                logVerbose("Started DB change notifier after sequence %" PRIu64, after);
            } else {
                logVerbose("Re-arming DB change notifier, after sequence %" PRIu64, after);
            }

            // Read changes from db change notifier, so it can fire again:
            SequenceTracker::Change changes[100];
            bool external;
            size_t n;
            while ((n = _dbNotifier->readChanges(changes, 100, external)) > 0)
                lastSequence = changes[n-1].sequence;
        });

        if (lastSequence > after && _currentEnumerator) {
            logVerbose("Hm, DB has changed to %" PRIu64 " already; triggering another run",
                       lastSequence);
            _dbChanged();
        }
    }


    void LiveQuerier::_stop() {
        _backgroundDB->use([&](DataFile *df) {
            _query = nullptr;
        });
        _currentEnumerator = nullptr;
        _database->sequenceTracker().use([&](SequenceTracker &st) {
            _dbNotifier.reset();
        });
    }


    // Threshold for rapidity of database changes. If it's been this long since the last change,
    // we re-query after the short delay. Otherwise we use the long delay. This allows for very
    // low latency if changes are not too rapid, while also not flooding the app with notifications
    // if changes are rapid.
    static constexpr delay_t kRapidChanges = chrono::milliseconds(250);

    static constexpr delay_t kShortDelay   = chrono::milliseconds(  0);
    static constexpr delay_t kLongDelay    = chrono::milliseconds(500);


    void LiveQuerier::_dbChanged() {
        auto now = clock::now();
        delay_t idleTime = now - _lastTime;
        _lastTime = now;

        delay_t delay = (idleTime <= kRapidChanges) ? kLongDelay : kShortDelay;
        logVerbose("DB changed after %.3f sec. Triggering query in %.3f secs",
                   idleTime.count(), delay.count());
        enqueueAfter(delay, &LiveQuerier::_run, _currentEnumerator->options());
    }

}
