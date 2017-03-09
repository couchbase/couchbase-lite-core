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
#include "DocumentMeta.hh"
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


struct C4QueryEnumInternal : public C4QueryEnumerator, InstanceCounted {
#if C4DB_THREADSAFE
    C4QueryEnumInternal(mutex &m)
    :_mutex(m)
#else
    C4QueryEnumInternal()
#endif
    {
        ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator));   // init public fields
    }

    virtual ~C4QueryEnumInternal() { }

    virtual bool next() {
        ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator));   // clear public fields
        return false;
    }

    virtual void close() noexcept { }

#if C4DB_THREADSAFE
    mutex &_mutex;
#endif
};

static C4QueryEnumInternal* asInternal(C4QueryEnumerator *e) {return (C4QueryEnumInternal*)e;}


bool c4queryenum_next(C4QueryEnumerator *e,
                      C4Error *outError) noexcept
{
    return tryCatch<bool>(outError, [&]{
        WITH_LOCK(asInternal(e));
        if (asInternal(e)->next())
            return true;
        clearError(outError);      // end of iteration is not an error
        return false;
    });
}


void c4queryenum_close(C4QueryEnumerator *e) noexcept {
    if (e) {
        WITH_LOCK(asInternal(e));
        asInternal(e)->close();
    }
}

void c4queryenum_free(C4QueryEnumerator *e) noexcept {
    c4queryenum_close(e);
    delete asInternal(e);
}


// This is the same as C4Query
struct c4Query : InstanceCounted {
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


struct C4DBQueryEnumerator : public C4QueryEnumInternal {
    C4DBQueryEnumerator(C4Query *query,
                        const QueryEnumerator::Options *options)
    :C4QueryEnumInternal(
#if C4DB_THREADSAFE
        query->database()->_mutex
#endif
    )
    ,_database(query->database())
    ,_enum(query->query(), options)
    ,_hasFullText(_enum.hasFullText())
    { }

    virtual bool next() override {
        if (!_enum.next())
            return C4QueryEnumInternal::next();
        docID = _enum.recordID();
        docSequence = _enum.sequence();
        DocumentMeta meta(_enum.meta());
        docFlags = meta.flags;
        revID = _revIDBuf = _database->documentFactory().revIDFromMeta(meta);

        if (_hasFullText) {
            auto &ft = _enum.fullTextTerms();
            fullTextTerms = (const C4FullTextTerm*)ft.data();
            fullTextTermCount = (uint32_t)ft.size();
        }
        return true;
    }

    alloc_slice getCustomColumns()          {return _enum.getCustomColumns();}
    alloc_slice getMatchedText()            {return _enum.getMatchedText();}

    virtual void close() noexcept override  {_enum.close();}

private:
    Retained<Database> _database;
    QueryEnumerator _enum;
    alloc_slice _revIDBuf;
    bool _hasFullText;
};


C4QueryEnumerator* c4query_run(C4Query *query,
                               const C4QueryOptions *options,
                               C4Slice encodedParameters,
                               C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        WITH_LOCK(query->database());
        QueryEnumerator::Options qeOpts;
        if (options) {
            qeOpts.skip = options->skip;
            qeOpts.limit = options->limit;
        }
        qeOpts.paramBindings = encodedParameters;
        return new C4DBQueryEnumerator(query, &qeOpts);
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


C4SliceResult c4queryenum_customColumns(C4QueryEnumerator *e) noexcept {
    return tryCatch<C4SliceResult>(nullptr, [&]{
        WITH_LOCK(asInternal(e));
        return sliceResult(((C4DBQueryEnumerator*)e)->getCustomColumns());
    });
}


C4SliceResult c4query_fullTextMatched(C4Query *query,
                                      C4Slice docID,
                                      C4SequenceNumber seq,
                                      C4Error *outError) noexcept
{
    return tryCatch<C4SliceResult>(outError, [&]{
        WITH_LOCK(query->database());
        return sliceResult(query->query()->getMatchedText(docID, seq));
    });
}


C4SliceResult c4queryenum_fullTextMatched(C4QueryEnumerator *e,
                                          C4Error *outError) noexcept
{
    return tryCatch<C4SliceResult>(outError, [&]{
        WITH_LOCK(asInternal(e));
        return sliceResult(((C4DBQueryEnumerator*)e)->getMatchedText());
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
        WITH_LOCK(database);
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
        WITH_LOCK(database);
        database->defaultKeyStore().deleteIndex((string)propertyPath,
                                                (KeyStore::IndexType)indexType);
    });
}
