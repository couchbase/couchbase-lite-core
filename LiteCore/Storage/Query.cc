//
//  Query.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/5/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Query.hh"
#include "KeyStore.hh"


namespace litecore {

    QueryEnumerator::QueryEnumerator(Query *query,
                                     const Options *options)
    :_impl(query->createEnumerator(options))
    { }


    bool QueryEnumerator::next() {
        if (_impl) {
            if (_impl->next(_recordID, _sequence))
                return true;
            _impl.reset();
        }
        return false;
    }

}
