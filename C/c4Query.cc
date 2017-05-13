//
//  c4Query.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/16/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "c4Internal.hh"
#include "c4Query.h"

#include "Database.hh"
#include "DataFile.hh"
#include "Query.hh"
#include "Record.hh"
#include <math.h>
#include <limits.h>
#include <mutex>
using namespace litecore;


#pragma mark COMMON CODE:


CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = {
    0,
    UINT_MAX,
    true
};


// This is the same as C4Query
struct c4Query : C4InstanceCounted {
    c4Query(Database *db, C4Slice queryExpression)
    :_database(db),
     _query(db->defaultKeyStore().compileQuery(queryExpression))
    { }

    Database* database() const      {return _database;}
    Query* query() const            {return _query.get();}

private:
    Retained<Database> _database;
    unique_ptr<Query> _query;
};


// Extension of C4QueryEnumerator
struct C4QueryEnumeratorImpl : public C4QueryEnumerator, C4InstanceCounted {
    C4QueryEnumeratorImpl(C4Query *query, const Query::Options *options)
    :_database(query->database())
    ,_enum(query->query()->createEnumerator(options))
    ,_hasFullText(_enum->hasFullText())
    {
        clearPublicFields();
    }

    void clearPublicFields() {
        ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator));
    }

    bool next() {
        if (!_enum->next()) {
            clearPublicFields();
            return false;
        }
        docID = _enum->recordID();
        docSequence = _enum->sequence();
        docFlags = (C4DocumentFlags)_enum->flags();
        revID = _revIDBuf = _database->documentFactory().revIDFromVersion(_enum->version());

        if (_hasFullText) {
            auto &ft = _enum->fullTextTerms();
            fullTextTerms = (const C4FullTextTerm*)ft.data();
            fullTextTermCount = (uint32_t)ft.size();
        } else {
            (fleece::Array::iterator&)columns = _enum->columns();
        }
        return true;
    }

    alloc_slice getMatchedText()            {return _enum->getMatchedText();}

    void close() noexcept                   {_enum.reset();}

private:
    Retained<Database> _database;
    unique_ptr<QueryEnumerator> _enum;
    alloc_slice _revIDBuf;
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


C4StringResult c4query_nameOfColumn(C4Query *query, unsigned col) noexcept {
    return tryCatch<C4StringResult>(nullptr, [&]{
        return sliceResult( query->query()->nameOfColumn(col) );
    });
}


C4QueryEnumerator* c4query_run(C4Query *query,
                               const C4QueryOptions *c4options,
                               C4Slice encodedParameters,
                               C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        Query::Options options;
        if (c4options) {
            options.skip = c4options->skip;
            options.limit = c4options->limit;
        }
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
                                      C4Slice docID,
                                      C4SequenceNumber seq,
                                      C4Error *outError) noexcept
{
    return tryCatch<C4SliceResult>(outError, [&]{
        return sliceResult(query->query()->getMatchedText(docID, seq));
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


void c4queryenum_close(C4QueryEnumerator *e) noexcept {
    if (e) {
        asInternal(e)->close();
    }
}

void c4queryenum_free(C4QueryEnumerator *e) noexcept {
    c4queryenum_close(e);
    delete asInternal(e);
}


C4SliceResult c4queryenum_fullTextMatched(C4QueryEnumerator *e,
                                          C4Error *outError) noexcept
{
    return tryCatch<C4SliceResult>(outError, [&]{
        return sliceResult(((C4QueryEnumeratorImpl*)e)->getMatchedText());
    });
}


#pragma mark - INDEXES:


bool c4db_createIndex(C4Database *database,
                      C4Slice propertyPath,
                      C4IndexType indexType,
                      const C4IndexOptions *indexOptions,
                      C4Error *outError) noexcept
{
    static_assert(sizeof(C4IndexOptions) == sizeof(KeyStore::IndexOptions),
                  "IndexOptions types must match");
    return tryCatch(outError, [&]{
        database->defaultKeyStore().createIndex((string)propertyPath,
                                                (KeyStore::IndexType)indexType,
                                                (const KeyStore::IndexOptions*)indexOptions);
    });
}


bool c4db_deleteIndex(C4Database *database,
                      C4Slice propertyPath,
                      C4IndexType indexType,
                      C4Error *outError) noexcept
{
    return tryCatch(outError, [&]{
        database->defaultKeyStore().deleteIndex((string)propertyPath,
                                                (KeyStore::IndexType)indexType);
    });
}
