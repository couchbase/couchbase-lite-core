//
// c4Query.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Query.hh"
#include "c4Database.hh"
#include "c4Index.h"
#include "c4Observer.h"
#include "c4Private.h"

#include "c4ExceptionUtils.hh"
#include "CollectionImpl.hh"
#include "c4QueryImpl.hh"
#include "c4Internal.hh"

#include "DatabaseImpl.hh"
#include "LiveQuerier.hh"
#include "SQLiteDataFile.hh"
#include "FleeceImpl.hh"


using namespace std;
using namespace fleece::impl;
using namespace litecore;


CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = { };


C4Query::C4Query(C4Collection *coll, C4QueryLanguage language, slice queryExpression)
:_database(asInternal(coll)->dbImpl())
,_query(_database->dataFile()->compileQuery(queryExpression,
                                            (QueryLanguage)language,
                                            &asInternal(coll)->keyStore()))
{ }


C4Query::~C4Query() = default;


Retained<C4Query> C4Query::newQuery(C4Collection *coll, C4QueryLanguage language, slice expr,
                                    int *outErrorPos)
{
    try {
        return retained(new C4Query(coll, language, expr));
    } catch (Query::parseError &x) {
        if (outErrorPos) {
            *outErrorPos = x.errorPosition;
        }
        throw;
    } catch (...) {
        if (outErrorPos) {
            *outErrorPos = -1;
        }
        throw;
    }
}


Retained<C4Query> C4Query::newQuery(C4Database *db, C4QueryLanguage language, slice expr,
                                    int *outErrorPos)
{
    return newQuery(db->getDefaultCollection(), language, expr, outErrorPos);
}


unsigned C4Query::columnCount() const noexcept {
    return _query->columnCount();
}


slice C4Query::columnTitle(unsigned column) const {
    auto &titles = _query->columnTitles();
    return (column < titles.size()) ? titles[column] : slice{};
}


alloc_slice C4Query::explain() const {
    return alloc_slice(_query->explain());
}


alloc_slice C4Query::fullTextMatched(const C4FullTextMatch &term) {
    return _query->getMatchedText((Query::FullTextTerm&)term);
}


alloc_slice C4Query::parameters() const noexcept {
    LOCK(_mutex);
    return _parameters;
}


void C4Query::setParameters(slice parameters) {
    LOCK(_mutex);
    _parameters = parameters;
    
    if (_bgQuerier) {
        _bgQuerier->changeOptions(_parameters);
    }
}


#pragma mark - ENUMERATOR:


Retained<QueryEnumerator> C4Query::_createEnumerator(const C4QueryOptions *c4options,
                                                     slice encodedParameters)
{
    Query::Options options(encodedParameters ? encodedParameters : parameters());
    return _query->createEnumerator(&options);
}


Retained<C4QueryEnumeratorImpl> C4Query::wrapEnumerator(QueryEnumerator *e) {
    return e ? new C4QueryEnumeratorImpl(_database, _query, e) : nullptr;
}


C4Query::Enumerator C4Query::run(const C4QueryOptions* C4NULLABLE opt, slice params) {
    return Enumerator(this, opt, params);
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


C4Query::Enumerator::Enumerator(Retained<litecore::QueryEnumerator> e)
:_enum(std::move(e))
{ }


C4Query::Enumerator::Enumerator(Enumerator &&c4e)
:_enum(std::move(c4e._enum))
,_query(std::move(c4e._query))
{ }


C4Query::Enumerator::~Enumerator() = default;

void C4Query::Enumerator::close() noexcept          {_enum = nullptr; _query = nullptr;}
int64_t C4Query::Enumerator::rowCount() const       {return _enum->getRowCount();}
bool C4Query::Enumerator::next()                    {return _enum->next();}
void C4Query::Enumerator::seek(int64_t rowIndex)    {_enum->seek(rowIndex);}


bool C4Query::Enumerator::restart() {
    Assert(_query);
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
        } else {
            // CBL-2459: For the second+ observers, get the current query result and notify if
            // the result is available. The current result will be reported via the callback
            // on the _bgQuerier's queue, which is the same queue where the _bgQuerier notifies
            // the updated query results to its delegate.
            //
            // While waiting for the current result, if there are new observers enabled,
            // add them to the _pendingObservers set so the current result can be notified to
            // all pending observers at once. Noted that if the delegate (See liveQuerierUpdated())
            // is called before the current result callback is called, the _pendingObservers
            // will be cleared by the delegate as they will be notified with the updated result
            // by the delegate.
            
            _pendingObservers.insert(obs);
            if (_pendingObservers.size() > 1)
                return;
            
            // Note: the callback is called from the _bgQuerier's queue.
            _bgQuerier->getCurrentResult([&](QueryEnumerator* qe, C4Error err) {
                set<C4QueryObserverImpl *> observers;
                {
                    LOCK(_mutex);
                    if (qe || err.code > 0) // Have a result to notify
                        observers = _pendingObservers;
                    _pendingObservers.clear();
                }
                if (observers.size() > 0)
                    this->notifyObservers(observers, qe, err);
            });
        }
    } else {
        _observers.erase(obs);
        _pendingObservers.erase(obs);
        if (_observers.empty() && _bgQuerier) {
            _bgQuerier->stop();
            _bgQuerier = nullptr;
            _bgQuerierDelegate = nullptr;
        }
    }
}


void C4Query::liveQuerierUpdated(QueryEnumerator *qe, C4Error err) {
    Retained<C4QueryEnumeratorImpl> c4e = wrapEnumerator(qe);
    set<C4QueryObserverImpl *> observers;
    {
        LOCK(_mutex);
        if (!_bgQuerier) {
            return;
        }

        // CBL-2336: Calling notify inside the lock could result
        // in a deadlock, but on the other hand not calling it
        // inside the lock could result in the callback coming back
        // to mutate the collection while we are using it (which, 
        // coincidentally, is why this deadlocks in the first place).
        // So to counteract this, make a copy and iterate over that.
        observers = _observers;
        
        // Clear pending observers as all pending observers are in observers and will be notified
        // with the update.
        _pendingObservers.clear();
    }
    
    notifyObservers(observers, qe, err);
}

void C4Query::notifyObservers(const set<C4QueryObserverImpl*> &observers,
                              QueryEnumerator *qe, C4Error err)
{
    for(auto &obs : observers) {
        obs->notify(c4e, err);
    }
}
