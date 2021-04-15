//
// C4Collection.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#include "c4Collection.hh"
#include "c4Query.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "betterassert.hh"


// NOTE: Most of C4Collection is implemented in its concrete subclass CollectionImpl.


using namespace litecore;


C4Collection::C4Collection(C4Database *db, slice name)
:_database(db)
,_name(name)
{ }


C4Database* C4Collection::getDatabase() {
    precondition(_database != nullptr);
    return _database;
}


const C4Database* C4Collection::getDatabase() const {
    precondition(_database != nullptr);
    return _database;
}


C4Document* C4Collection::documentContainingValue(FLValue value) noexcept {
    auto doc = VectorDocumentFactory::documentContaining(value);
    if (!doc)
        doc = TreeDocumentFactory::documentContaining(value);
    return doc;
}


Retained<C4Query> C4Collection::newQuery(C4QueryLanguage language, slice expr,int *errPos) const {
    return C4Query::newQuery(const_cast<C4Collection*>(this), language, expr, errPos);
}
