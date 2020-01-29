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
#include "c4Index.h"
#include "c4Observer.h"
#include "c4ExceptionUtils.hh"
#include "c4Database.hh"

#include "LiveQuerier.hh"
#include "SQLiteDataFile.hh"
#include "Query.hh"
#include "Record.hh"
#include "Timer.hh"
#include "InstanceCounted.hh"
#include "StringUtil.hh"
#include "FleeceImpl.hh"
#include <list>
#include <math.h>
#include <limits.h>
#include <mutex>
#include <set>

using namespace std;
using namespace litecore;
using namespace fleece::impl;


#define LOCK(MUTEX)     lock_guard<mutex> _lock(MUTEX)


CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = {
    true
};


#pragma mark QUERY ENUMERATOR:


// Extension of C4QueryEnumerator
struct C4QueryEnumeratorImpl : public RefCounted,
                               public C4QueryEnumerator,
                               fleece::InstanceCountedIn<C4QueryEnumerator>
{
    C4QueryEnumeratorImpl(Database *database, Query *query, QueryEnumerator *e)
    :_database(database)
    ,_query(query)
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
        QueryEnumerator* newEnum = enumerator().refresh(_query);
        if (newEnum)
            return retain(new C4QueryEnumeratorImpl(_database, _query, newEnum));
        else
            return nullptr;
    }

    void close() noexcept {
        _enum = nullptr;
    }

    bool usesEnumerator(QueryEnumerator *e) const     {return e == _enum;}

private:
    Retained<Database> _database;
    Retained<Query> _query;
    Retained<QueryEnumerator> _enum;
    bool _hasFullText;
};

static C4QueryEnumeratorImpl* asInternal(C4QueryEnumerator *e) {return (C4QueryEnumeratorImpl*)e;}


#pragma mark - QUERY OBSERVER:


// This is the same as C4QueryObserver.
// Instances are not directly heap-allocated; they're managed by a std::list (c4Query::observers).
class c4QueryObserver : public fleece::InstanceCounted {
public:
    c4QueryObserver(C4Query *query, C4QueryObserverCallback callback, void* context)
    :_query(c4query_retain(query)), _callback(callback), _context(context)
    { }

    ~c4QueryObserver()              {c4query_release(_query);}

    C4Query* query() const          {return _query;}

    // called on a background thread
    void notify(C4QueryEnumeratorImpl *e, C4Error err) noexcept {
        {
            LOCK(_mutex);
            _currentEnumerator = e;
            _currentError = err;
        }
        _callback(this, _query, _context);
    }

    C4QueryEnumerator* currentEnumerator(C4Error *outError) {
        LOCK(_mutex);
        _lastEnumerator = _currentEnumerator;   // keep it alive till the next call
        if (!_currentEnumerator && outError)
            *outError = _currentError;
        return _currentEnumerator;
    }

private:
    C4Query* const                  _query;
    C4QueryObserverCallback const   _callback;
    void* const                     _context;
    mutable mutex                   _mutex;
    Retained<C4QueryEnumeratorImpl> _lastEnumerator;
    Retained<C4QueryEnumeratorImpl> _currentEnumerator;
    C4Error                         _currentError {};
};


#pragma mark - QUERY:


// This is the same as C4Query
struct c4Query : public RefCounted, public fleece::InstanceCountedIn<c4Query>, LiveQuerier::Delegate {
    c4Query(Database *db, C4QueryLanguage language, C4Slice queryExpression)
    :_database(db)
    ,_query(db->defaultKeyStore().compileQuery(queryExpression, (QueryLanguage)language))
    { }

    Database* database() const              {return _database;}
    Query* query() const                    {return _query;}
    alloc_slice parameters() const          {return _parameters;}
    void setParameters(slice parameters)    {_parameters = parameters;}

    Retained<C4QueryEnumeratorImpl> createEnumerator(const C4QueryOptions *c4options, slice encodedParameters) {
        Query::Options options(encodedParameters ? encodedParameters : _parameters);
        return wrapEnumerator( _query->createEnumerator(&options) );
    }

    Retained<C4QueryEnumeratorImpl> wrapEnumerator(QueryEnumerator *e) {
        return e ? new C4QueryEnumeratorImpl(_database, _query, e) : nullptr;
    }

    //// Observing:
    

    void enableObserver(c4QueryObserver *obs, bool enable) {
        LOCK(_mutex);
        if (enable) {
            _observers.insert(obs);
            if (!_bgQuerier) {
                _bgQuerier = new LiveQuerier(_database, _query, true, this);
                _bgQuerier->run(_parameters);
            }
        } else {
            _observers.erase(obs);
            if (_observers.empty() && _bgQuerier) {
                _bgQuerier->stop();
                _bgQuerier = nullptr;
            }
        }
    }

    // called on a background thread!
    void liveQuerierUpdated(QueryEnumerator *qe, C4Error err) override {
        Retained<C4QueryEnumeratorImpl> c4e = wrapEnumerator(qe);
        LOCK(_mutex);
        if (!_bgQuerier)
            return;
        for (auto &obs : _observers)
            obs->notify(c4e, err);
    }

private:
    Retained<Database> _database;
    Retained<Query> _query;
    alloc_slice _parameters;

    mutable mutex _mutex;
    Retained<LiveQuerier> _bgQuerier;
    std::set<c4QueryObserver*> _observers;
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
        return retain(query->createEnumerator(c4options, encodedParameters).get());
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

void c4queryenum_release(C4QueryEnumerator *e) noexcept {
    release(asInternal(e));
}


#pragma mark - QUERY OBSERVER API:


C4QueryObserver* c4queryobs_create(C4Query *query, C4QueryObserverCallback cb, void *ctx) C4API {
    return new C4QueryObserver(query, cb, ctx);
}

void c4queryobs_setEnabled(C4QueryObserver *obs, bool enabled) C4API {
    obs->query()->enableObserver(obs, enabled);
}

void c4queryobs_free(C4QueryObserver* obs) C4API {
    if (obs) {
        obs->query()->enableObserver(obs, false);
        delete obs;
    }
}

C4QueryEnumerator* c4queryobs_getEnumerator(C4QueryObserver *obs, C4Error *outError) C4API {
    return obs->currentEnumerator(outError);
}


#pragma mark - INDEXES:


bool c4db_createIndex(C4Database *database,
                      C4Slice name,
                      C4Slice indexSpecJSON,
                      C4IndexType indexType,
                      const C4IndexOptions *indexOptions,
                      C4Error *outError) noexcept
{
    static_assert(sizeof(C4IndexOptions) == sizeof(IndexSpec::Options),
                  "IndexSpec::Options types must match");
    return tryCatch(outError, [&]{
        database->defaultKeyStore().createIndex(slice(name),
                                                indexSpecJSON,
                                                (IndexSpec::Type)indexType,
                                                (const IndexSpec::Options*)indexOptions);
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


C4SliceResult c4db_getIndexRows(C4Database* database, C4String indexName, C4Error* outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        int64_t rowCount;
        alloc_slice rows;
        ((SQLiteDataFile*)database->dataFile())->inspectIndex(indexName, rowCount, &rows);
        return C4SliceResult(rows);
    });
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
