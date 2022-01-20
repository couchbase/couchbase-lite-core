//
// C4Collection.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Collection.hh"
#include "c4Query.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "betterassert.hh"


// NOTE: Most of C4Collection is implemented in its concrete subclass CollectionImpl.


using namespace litecore;


C4Collection::C4Collection(C4Database *db, C4ScopeID scope, slice name)
:_database(db)
,_scope(scope)
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
