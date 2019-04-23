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

#include "Database.hh"
#include "DataFile.hh"
#include "Query.hh"
#include "Record.hh"
#include "InstanceCounted.hh"
#include "FleeceImpl.hh"
#include <math.h>
#include <limits.h>
#include <mutex>

using namespace litecore;
using namespace fleece::impl;


#pragma mark COMMON CODE:


CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = {
    true
};


// This is the same as C4Query
struct c4Query : fleece::InstanceCounted {
    c4Query(Database *db, C4Slice queryExpression)
    :_database(db),
     _query(db->defaultKeyStore().compileQuery(queryExpression))
    { }

    Database* database() const      {return _database;}
    Query* query() const            {return _query.get();}

private:
    Retained<Database> _database;
    Retained<Query> _query;
};


// Extension of C4QueryEnumerator
struct C4QueryEnumeratorImpl : public C4QueryEnumerator, fleece::InstanceCountedIn<C4QueryEnumerator> {
    C4QueryEnumeratorImpl(Database *database, QueryEnumerator *e)
    :_database(database)
    ,_enum(e)
    ,_hasFullText(_enum->hasFullText())
    {
        clearPublicFields();
    }

    C4QueryEnumeratorImpl(C4Query *query, const Query::Options *options)
    :C4QueryEnumeratorImpl(query->database(), query->query()->createEnumerator(options))
    { }

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
        QueryEnumerator* newEnum = enumerator().refresh();
        if (newEnum)
            return new C4QueryEnumeratorImpl(_database, newEnum);
        else
            return nullptr;
    }

    void close() noexcept {
        _enum.reset();
    }

private:
    Retained<Database> _database;
    unique_ptr<QueryEnumerator> _enum;
    bool _hasFullText;
};


static C4QueryEnumeratorImpl* asInternal(C4QueryEnumerator *e) {return (C4QueryEnumeratorImpl*)e;}


#pragma mark - QUERY:


C4Query* c4query_new(C4Database *database,
                     C4Slice expression,
                     C4Error *outError) noexcept
{
    return tryCatch<C4Query*>(outError, [&]{
        return new c4Query(database, expression);
    });
}


void c4query_free(C4Query *query) noexcept {
    delete query;
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


C4QueryEnumerator* c4query_run(C4Query *query,
                               const C4QueryOptions *c4options,
                               C4Slice encodedParameters,
                               C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        Query::Options options;
        options.paramBindings = encodedParameters;
        return new C4QueryEnumeratorImpl(query, &options);
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


#pragma mark - QUERY ENUMERATOR:


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


void c4queryenum_close(C4QueryEnumerator *e) noexcept {
    if (e) {
        asInternal(e)->close();
    }
}

void c4queryenum_free(C4QueryEnumerator *e) noexcept {
    delete asInternal(e);
}


#pragma mark - INDEXES:


bool c4db_createIndex(C4Database *database,
                      C4Slice name,
                      C4Slice propertyPath,
                      C4IndexType indexType,
                      const C4IndexOptions *indexOptions,
                      C4Error *outError) noexcept
{
    static_assert(sizeof(C4IndexOptions) == sizeof(KeyStore::IndexOptions),
                  "IndexOptions types must match");
    return tryCatch(outError, [&]{
        database->defaultKeyStore().createIndex({string(slice(name)),
                                                 (KeyStore::IndexType)indexType,
                                                 alloc_slice(propertyPath)},
                                                (const KeyStore::IndexOptions*)indexOptions);
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
