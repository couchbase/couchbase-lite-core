//
//  Query.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/5/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Query.hh"
#include "KeyStore.hh"


namespace litecore {

    QueryEnumerator::QueryEnumerator(Query *query,
                                     const Options *options)
    :_impl(query->createEnumerator(options))
    { }


    bool QueryEnumerator::next() {
        if (_impl) {
            if (_impl->next(_docID, _sequence))
                return true;
            _impl.reset();
        }
        return false;
    }

}
