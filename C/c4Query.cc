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

#include "c4Query.hh"
#include "c4Database.hh"
#include "c4Index.h"
#include "c4Observer.h"
#include "c4Private.h"

#include "c4ExceptionUtils.hh"
#include "c4QueryImpl.hh"
#include "c4Internal.hh"

#include "DatabaseImpl.hh"
#include "LiveQuerier.hh"
#include "SQLiteDataFile.hh"
#include "FleeceImpl.hh"


using namespace std;
using namespace fleece::impl;
using namespace litecore;
using namespace c4Internal;


CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = { };


C4Query::C4Query(C4Database *db, C4QueryLanguage language, C4Slice queryExpression)
:_database(c4Internal::asInternal(db))
,_query(_database->defaultKeyStore().compileQuery(queryExpression, (QueryLanguage)language))
{ }


Retained<C4Query> C4Database::newQuery(C4QueryLanguage language, C4Slice queryExpression,
                                       int *outErrorPos) {
    try {
        return retained(new C4Query(this, language, queryExpression));
    } catch (Query::parseError &x) {
        if (outErrorPos) {
            *outErrorPos = x.errorPosition;
            return nullptr;
        } else {
            throw;
        }
    }
}


unsigned C4Query::columnCount() const noexcept {
    return _query->columnCount();
}


slice C4Query::columnTitle(unsigned column) const {
    auto &titles = _query->columnTitles();
    return (column < titles.size()) ? slice(titles[column]) : slice{};
}


alloc_slice C4Query::explain() const {
    return alloc_slice(_query->explain());
}


alloc_slice C4Query::fullTextMatched(const C4FullTextMatch &term) {
    return _query->getMatchedText((Query::FullTextTerm&)term);
}


#pragma mark - ENUMERATOR:


Retained<QueryEnumerator> C4Query::_createEnumerator(const C4QueryOptions *c4options,
                                                     slice encodedParameters)
{
    Query::Options options(encodedParameters ? encodedParameters : _parameters);
    return _query->createEnumerator(&options);
}


Retained<C4QueryEnumeratorImpl> C4Query::wrapEnumerator(QueryEnumerator *e) {
    return e ? new C4QueryEnumeratorImpl(_database, _query, e) : nullptr;
}


C4QueryEnumerator* C4Query::createEnumerator(const C4QueryOptions *c4options,
                                             slice encodedParameters)
{
    auto e = _createEnumerator(c4options, encodedParameters);
    return wrapEnumerator(e).detach();
}


C4Query::Enumerator::Enumerator(C4Query *query,
                                const C4QueryOptions *c4options,
                                slice encodedParameters)
:_enum(query->_createEnumerator(c4options, encodedParameters))
,_query(query->_query)
{ }


C4Query::Enumerator::~Enumerator() = default;

void C4Query::Enumerator::close() noexcept          {_enum = nullptr; _query = nullptr;}
int64_t C4Query::Enumerator::rowCount() const       {return _enum->getRowCount();}
bool C4Query::Enumerator::next()                    {return _enum->next();}
void C4Query::Enumerator::seek(int64_t rowIndex)    {_enum->seek(rowIndex);}


bool C4Query::Enumerator::restart() {
    auto newEnum = _enum->refresh(_query);
    if (!newEnum)
        return false;
    _enum = newEnum;
    return true;
}


FLArrayIterator C4Query::Enumerator::columns() const {
    // (FLArrayIterator is binary-compatible with Array::iterator)
    static_assert(sizeof(FLArrayIterator) == sizeof(Array::iterator));
    auto cols = _enum->columns();
    return (FLArrayIterator&)cols;
}


FLValue C4Query::Enumerator::column(unsigned i) const {
    if (i < 64 && (_enum->missingColumns() & (1ull << i)))
        return nullptr;
    return (FLValue)_enum->columns()[i];
}


unsigned C4Query::Enumerator::fullTextMatchCount() const {
    return (unsigned)_enum->fullTextTerms().size();
}


C4FullTextMatch C4Query::Enumerator::fullTextMatch(unsigned i) const {
    // (C4FullTextMatch is binary-compatible with Query::FullTextTerm)
    static_assert(sizeof(C4FullTextMatch) == sizeof(Query::FullTextTerm));
    return (C4FullTextMatch&)_enum->fullTextTerms()[i];
}


#pragma mark - OBSERVER:


class C4Query::LiveQuerierDelegate : public LiveQuerier::Delegate {
public:
    LiveQuerierDelegate(C4Query *query) :_query(query) { }

    // called on a background thread!
    void liveQuerierUpdated(QueryEnumerator *qe, C4Error err) override {
        _query->liveQuerierUpdated(qe, err);
    }

    C4Query* _query;
};


std::unique_ptr<C4QueryObserver> C4Query::observe(std::function<void(C4QueryObserver*)> callback) {
    return make_unique<C4QueryObserverImpl>(this, callback);
}


void C4Query::enableObserver(C4QueryObserverImpl *obs, bool enable) {
    LOCK(_mutex);
    if (enable) {
        _observers.insert(obs);
        if (!_bgQuerier) {
            _bgQuerierDelegate = make_unique<LiveQuerierDelegate>(this);
            _bgQuerier = new LiveQuerier(_database, _query, true, _bgQuerierDelegate.get());
            _bgQuerier->start(_parameters);
        }
    } else {
        _observers.erase(obs);
        if (_observers.empty() && _bgQuerier) {
            _bgQuerier->stop();
            _bgQuerier = nullptr;
            _bgQuerierDelegate = nullptr;
        }
    }
}


void C4Query::liveQuerierUpdated(QueryEnumerator *qe, C4Error err) {
    Retained<C4QueryEnumeratorImpl> c4e = wrapEnumerator(qe);
    LOCK(_mutex);
    if (!_bgQuerier)
        return;
    for (auto &obs : _observers)
        obs->notify(c4e, err);
}
