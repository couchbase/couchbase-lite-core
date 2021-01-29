//
//  c4QueryObserver.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/20.
//  Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "c4Observer.h"
#include "c4Query.h"

#include "c4Internal.hh"
#include "c4QueryEnumeratorImpl.hh"

#include "InstanceCounted.hh"

#include <mutex>
#include <utility>


using namespace litecore;
// This is the definition of the C4Query type in the public C API,
// hence it must be in the global namespace.
struct C4QueryObserver : public fleece::InstanceCounted {
public:
    C4QueryObserver(C4Query *query, C4QueryObserverCallback callback, void* context)
    :_query(c4query_retain(query))
    ,_callback(callback)
    ,_context(context)
    { }

    ~C4QueryObserver() {
        c4query_release(_query);
    }

    C4Query* query() const {
        return _query;
    }

    // called on a background thread
    void notify(C4QueryEnumeratorImpl *e, C4Error err) noexcept {
        {
            LOCK(_mutex);
            _currentEnumerator = e;
            _currentError = err;
        }
        _callback(this, _query, _context);
    }

    Retained<C4QueryEnumeratorImpl> currentEnumerator(bool forget, C4Error *outError) {
        LOCK(_mutex);
        if (outError)
            *outError = _currentError;
        if (forget)
            return std::move(_currentEnumerator);
        else
            return _currentEnumerator;
    }

private:
    C4Query* const                  _query;
    C4QueryObserverCallback const   _callback;
    void* const                     _context;
    mutable std::mutex              _mutex;
    Retained<C4QueryEnumeratorImpl> _currentEnumerator;
    C4Error                         _currentError {};
};



