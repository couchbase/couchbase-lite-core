//
// c4Query.hh
//
// Copyright (c) 2020 Couchbase, Inc All rights reserved.
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

using namespace litecore;
using namespace c4Internal;


// This is the definition of the C4Query type in the public C API,
// hence it must be in the global namespace.
struct C4Query : public RefCounted, public fleece::InstanceCountedIn<C4Query>, LiveQuerier::Delegate {
    C4Query(Database *db, C4QueryLanguage language, C4Slice queryExpression)
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

    void enableObserver(C4QueryObserver *obs, bool enable) {
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

    Retained<LiveQuerier> _bgQuerier;
    std::set<C4QueryObserver*> _observers;
    mutable std::mutex _mutex;
};

