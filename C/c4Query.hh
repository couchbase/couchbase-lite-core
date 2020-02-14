//
//  c4Query.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/20.
//  Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "c4Query.h"

#include "c4Database.hh"
#include "c4QueryEnumeratorImpl.hh"
#include "c4QueryObserver.hh"
#include "LiveQuerier.hh"

#include "InstanceCounted.hh"
#include "RefCounted.hh"

#include <mutex>
#include <set>

using namespace std;
using namespace litecore;
using namespace c4Internal;


// This is the definition of the C4Query type in the public C API,
// hence it must be in the global namespace.
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

    void enableObserver(c4QueryObserver *obs, bool enable) {
        LOCK(_mutex);
        if (enable) {
            _observers.insert(obs);
            if (!_bgQuerier) {
                _bgQuerier = new LiveQuerier(_database, _query, true, this);
                _bgQuerier->start(_parameters);
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
    set<c4QueryObserver*> _observers;
};

