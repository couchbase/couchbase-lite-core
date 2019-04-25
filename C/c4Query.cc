//
// c4Query.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "c4Internal.hh"
#include "c4Query.h"
#include "c4Observer.h"
#include "c4ExceptionUtils.hh"

#include "Database.hh"
#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "Query.hh"
#include "n1ql_parser.hh"
#include "Record.hh"
#include "SequenceTracker.hh"
#include "Timer.hh"
#include "InstanceCounted.hh"
#include "StringUtil.hh"
#include "FleeceImpl.hh"
#include <list>
#include <math.h>
#include <limits.h>
#include <mutex>

using namespace std;
using namespace std::placeholders;
using namespace litecore;
using namespace fleece::impl;


// How long an observed query waits after a DB change before re-running
static constexpr actor::Timer::duration kLatency = chrono::milliseconds(500);


#pragma mark COMMON CODE:


CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = {
    true
};


// Extension of C4QueryEnumerator
struct C4QueryEnumeratorImpl : public RefCounted,
                               public C4QueryEnumerator,
                               fleece::InstanceCountedIn<C4QueryEnumerator> {
    C4QueryEnumeratorImpl(Database *database, QueryEnumerator *e)
    :_database(database)
    ,_enum(e)
    ,_hasFullText(_enum->hasFullText())
    {
        clearPublicFields();
    }

    QueryEnumerator& enumerator() const {
        if (!_enum)
            error::_throw(error::InvalidParameter, "Query enumerator has been closed");
        return *_enum;
    }

    int64_t getRowCount() const         {return enumerator().getRowCount();}

    bool next() {
        if (!enumerator().next()) {
            clearPublicFields();
            return false;
        }
        populatePublicFields();
        return true;
    }

    void seek(int64_t rowIndex) {
        enumerator().seek(rowIndex);
        if (rowIndex >= 0)
            populatePublicFields();
        else
            clearPublicFields();
    }

    void clearPublicFields() {
        ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator));
    }

    void populatePublicFields() {
        static_assert(sizeof(C4FullTextMatch) == sizeof(Query::FullTextTerm),
                      "C4FullTextMatch does not match Query::FullTextTerm");
        (Array::iterator&)columns = _enum->columns();
        missingColumns = _enum->missingColumns();
        if (_hasFullText) {
            auto &ft = _enum->fullTextTerms();
            fullTextMatches = (const C4FullTextMatch*)ft.data();
            fullTextMatchCount = (uint32_t)ft.size();
        }
    }

    C4QueryEnumeratorImpl* refresh() {
        QueryEnumerator* newEnum = enumerator().refresh();
        if (newEnum)
            return retain(new C4QueryEnumeratorImpl(_database, newEnum));
        else
            return nullptr;
    }

    void close() noexcept {
        _enum = nullptr;
    }

private:
    Retained<Database> _database;
    Retained<QueryEnumerator> _enum;
    bool _hasFullText;
};

static C4QueryEnumeratorImpl* asInternal(C4QueryEnumerator *e) {return (C4QueryEnumeratorImpl*)e;}


// This is the same as C4QueryObserver
struct c4QueryObserver : fleece::InstanceCounted {
    c4QueryObserver(C4Query *query, C4QueryObserverCallback callback, void* context)
    :_query(c4query_retain(query)), _callback(callback), _context(context)
    { }

    void operator() (C4QueryEnumerator *e, C4Error err) noexcept {
        _callback(this, e, err, _context);
    }

    ~c4QueryObserver() {
        c4query_release(_query);
    }

    C4Query* const                  _query;
    C4QueryObserverCallback const   _callback;
    void* const                     _context;
};


// This is the same as C4Query
struct c4Query : public RefCounted, fleece::InstanceCounted {
    c4Query(Database *db, C4QueryLanguage language, C4Slice queryExpression)
    :_database(db)
    ,_query(db->defaultKeyStore().compileQuery(queryExpression, (QueryLanguage)language))
    ,_refreshTimer(bind(&c4Query::timerFired, this))
    { }

    Database* database() const              {return _database;}
    Query* query() const                    {return _query;}
    alloc_slice parameters() const          {return _parameters;}
    void setParameters(slice parameters)    {_parameters = parameters;}

    QueryEnumerator* createEnumerator(const C4QueryOptions *c4options, slice encodedParameters) {
        Query::Options options(encodedParameters ? encodedParameters : _parameters);
        return _query->createEnumerator(&options);
    }

    c4QueryObserver* createObserver(C4QueryObserverCallback callback, void *context) {
        if (_observers.empty())
            beginObserving();
        _observers.emplace_back(this, callback, context);
        return &_observers.back();
    }

    void freeObserver(c4QueryObserver *obs) {
        _observers.remove_if([obs](const c4QueryObserver &o) {return &o == obs;});
        if (_observers.empty())
            endObserving();
    }

    void callObservers(QueryEnumerator *e, const error &err) {
        Retained<C4QueryEnumeratorImpl> c4e;
        if (e)
            c4e = new C4QueryEnumeratorImpl(_database, e);
        C4Error c4err;
        recordException(err, &c4err);

        for (auto &obs : _observers)
            obs(c4e, c4err);
    }

    void beginObserving() {
        // The first time we just run the query directly (in the background):
        _database->backgroundDatabase()->runQuery(_query, _parameters,
            [&](Retained<QueryEnumerator> e, error err) {
                // When it's ready, notify the observers:
                callObservers(e, err);
                if (e) {
                    _currentEnumerator = e;
                    // And set up a database observer to listen for changes:
                    _dbNotifier.reset(new DatabaseChangeNotifier(_database->sequenceTracker(),
                                                                 bind(&c4Query::dbChanged, this, _1),
                                                                 e->lastSequence()));
                }
            });
    }

    void dbChanged(DatabaseChangeNotifier&) {
        _refreshTimer.fireAfter(kLatency);
    }

    void timerFired() {
        _database->backgroundDatabase()->refreshQuery(_currentEnumerator,
              [&](Retained<QueryEnumerator> e, error err) {
                  if (e || err.code != 0) {
                      // When the query result changes, or on error, notify the observers:
                      callObservers(e, err);
                  }
                  if (e)
                      _currentEnumerator = e;
                  // Read (and ignore) changes from notifier, so it can fire again:
                  bool external;
                  while (_dbNotifier->readChanges(nullptr, 1000, external))
                      ;
              });
    }

    void endObserving() {
        _dbNotifier.reset();
    }

private:
    Retained<Database> _database;
    Retained<Query> _query;
    alloc_slice _parameters;

    Retained<QueryEnumerator> _currentEnumerator;
    std::list<c4QueryObserver> _observers;
    unique_ptr<DatabaseChangeNotifier> _dbNotifier;
    sequence_t _lastSequence {0};
    actor::Timer _refreshTimer;
};


#pragma mark - QUERY API:


C4Query* c4query_new2(C4Database *database,
                      C4QueryLanguage language,
                      C4Slice expression,
                      int *outErrorPos,
                      C4Error *outError) noexcept
{
    if (outErrorPos)
        *outErrorPos = -1;
    return tryCatch<C4Query*>(outError, [&]{
        try {
            return retain(new c4Query(database, language, expression));
        } catch (Query::parseError &x) {
            if (outErrorPos)
                *outErrorPos = x.errorPosition;
            throw;
        }
    });
}

C4Query* c4query_new(C4Database *database C4NONNULL, C4String expression, C4Error *error) C4API {
    return c4query_new2(database, kC4JSONQuery, expression, nullptr, error);
}


C4Query* c4query_retain(C4Query *query) C4API {
    return retain(query);
}


void c4query_free(C4Query *query) noexcept {
    release(query);
}


unsigned c4query_columnCount(C4Query *query) noexcept {
    return query->query()->columnCount();
}


FLString c4query_columnTitle(C4Query *query, unsigned column) C4API {
    auto &titles = query->query()->columnTitles();
    if (column >= titles.size())
        return {};
    return slice(titles[column]);
}


void c4query_setParameters(C4Query *query, C4String encodedParameters) C4API {
    query->setParameters(encodedParameters);
}


C4QueryEnumerator* c4query_run(C4Query *query,
                               const C4QueryOptions *c4options,
                               C4Slice encodedParameters,
                               C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        return retain(new C4QueryEnumeratorImpl(query->database(),
                                         query->createEnumerator(c4options, encodedParameters)));
    });
}



C4StringResult c4query_explain(C4Query *query) noexcept {
    return tryCatch<C4StringResult>(nullptr, [&]{
        string result = query->query()->explain();
        if (result.empty())
            return C4StringResult{};
        return sliceResult(result);
    });
}


C4SliceResult c4query_fullTextMatched(C4Query *query,
                                      const C4FullTextMatch *term,
                                      C4Error *outError) noexcept
{
    return tryCatch<C4SliceResult>(outError, [&]{
        return C4SliceResult(query->query()->getMatchedText(*(Query::FullTextTerm*)term));
    });
}


#pragma mark - QUERY ENUMERATOR API:


bool c4queryenum_next(C4QueryEnumerator *e,
                      C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        if (asInternal(e)->next())
            return true;
        clearError(outError);      // end of iteration is not an error
        return false;
    });
}


bool c4queryenum_seek(C4QueryEnumerator *e,
                      int64_t rowIndex,
                      C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        asInternal(e)->seek(rowIndex);
        return true;
    });
}


int64_t c4queryenum_getRowCount(C4QueryEnumerator *e,
                                 C4Error *outError) noexcept
{
    try {
        return asInternal(e)->getRowCount();
    } catchError(outError)
    return -1;
}



C4QueryEnumerator* c4queryenum_refresh(C4QueryEnumerator *e,
                                       C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        clearError(outError);
        return asInternal(e)->refresh();
    });
}


C4QueryEnumerator* c4queryenum_retain(C4QueryEnumerator *e) C4API {
    return retain(asInternal(e));
}


void c4queryenum_close(C4QueryEnumerator *e) noexcept {
    if (e) {
        asInternal(e)->close();
    }
}

void c4queryenum_free(C4QueryEnumerator *e) noexcept {
    release(asInternal(e));
}


#pragma mark - QUERY OBSERVER API:


C4QueryObserver* c4queryobs_create(C4Query *query, C4QueryObserverCallback cb, void *ctx) C4API {
    return query->createObserver(cb, ctx);
}

void c4queryobs_free(C4QueryObserver* obs) C4API {
    if (obs)
        obs->_query->freeObserver(obs);
}



#pragma mark - INDEXES:


bool c4db_createIndex(C4Database *database,
                      C4Slice name,
                      C4Slice propertyPath,
                      C4IndexType indexType,
                      const C4IndexOptions *indexOptions,
                      C4Error *outError) noexcept
{
    static_assert(sizeof(C4IndexOptions) == sizeof(KeyStore::IndexOptions),
                  "IndexOptions types must match");
    return tryCatch(outError, [&]{
        database->defaultKeyStore().createIndex({string(slice(name)),
                                                 (KeyStore::IndexType)indexType,
                                                 alloc_slice(propertyPath)},
                                                (const KeyStore::IndexOptions*)indexOptions);
    });
}


bool c4db_deleteIndex(C4Database *database,
                      C4Slice name,
                      C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{
        database->defaultKeyStore().deleteIndex(toString(name));
    });
}

static C4SliceResult getIndexes(C4Database* database, bool fullInfo, C4Error* outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        Encoder enc;
        enc.beginArray();
        for (const auto &spec : database->defaultKeyStore().getIndexes()) {
            if (fullInfo) {
                enc.beginDictionary();
                enc.writeKey("name"); enc.writeString(spec.name);
                enc.writeKey("type"); enc.writeInt(spec.type);
                enc.writeKey("expr"); enc.writeString(spec.expressionJSON);
                enc.endDictionary();
            } else {
                enc.writeString(spec.name);
            }
        }
        enc.endArray();
        return C4SliceResult(enc.finish());
    });
}

C4SliceResult c4db_getIndexes(C4Database* database, C4Error* outError) noexcept {
    return getIndexes(database, false, outError);
}


C4SliceResult c4db_getIndexesInfo(C4Database* database, C4Error* outError) noexcept {
    return getIndexes(database, true, outError);
}


// Stubs for functions only available in EE:
#ifndef COUCHBASE_ENTERPRISE
#include "c4PredictiveQuery.h"

void c4pred_registerModel(const char *name, C4PredictiveModel model) C4API {
    abort();
}

bool c4pred_unregisterModel(const char *name) C4API {
    abort();
}

#endif
