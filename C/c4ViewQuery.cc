//
//  c4ViewQuery.cc
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
#include "c4View.h"
#include "c4DBQuery.h"

#include "c4ViewInternal.hh"
#include "Database.hh"
#include "c4KeyInternal.hh"

#include "DataFile.hh"
#include "Query.hh"
#include "Collatable.hh"
#include "DocumentMeta.hh"
#include <math.h>
#include <limits.h>
#include <mutex>
using namespace litecore;


#pragma mark COMMON CODE:


// C4KeyReader is really identical to CollatableReader, which itself consists of nothing but
// a slice.
static inline C4KeyReader asKeyReader(const CollatableReader &r) {
    return *(C4KeyReader*)&r;
}


CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = {
    0,
    UINT_MAX,
    false,
    true,
    true,
    true
};


class C4ReduceAdapter : public ReduceFunction {
public:
    C4ReduceAdapter(const C4ReduceFunction *callback)
    :_callback(*callback)
    { }

    void operator() (CollatableReader key, slice value) override {
        c4Key k(toc4slice(key.data()));
        _callback.accumulate(_callback.context, &k, toc4slice(value));
    }

    slice reducedValue() override {
        return _callback.reduce(_callback.context);
    }

private:
    C4ReduceFunction _callback;
};


static IndexEnumerator::Options convertOptions(const C4QueryOptions *c4options) {
    if (!c4options)
        c4options = &kC4DefaultQueryOptions;
    IndexEnumerator::Options options;
    options.skip = (unsigned)c4options->skip;
    options.limit = (unsigned)c4options->limit;
    options.descending = c4options->descending;
    options.inclusiveStart = c4options->inclusiveStart;
    options.inclusiveEnd = c4options->inclusiveEnd;
    if (c4options->reduce)
        options.reduce = new C4ReduceAdapter(c4options->reduce);    // must be freed afterwards
    options.groupLevel = c4options->groupLevel;
    return options;
}


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


struct C4ViewQueryEnumInternal : public C4QueryEnumInternal {
    C4ViewQueryEnumInternal(C4View *view)
    :C4QueryEnumInternal(
#if C4DB_THREADSAFE
        view->_mutex
#endif
    ),
     _view(view)
    { }

    Retained<C4View> _view;
};



#pragma mark MAP/REDUCE QUERIES:


struct C4MapReduceEnumerator : public C4ViewQueryEnumInternal {
    C4MapReduceEnumerator(C4View *view,
                        Collatable startKey, slice startKeyDocID,
                        Collatable endKey, slice endKeyDocID,
                        const IndexEnumerator::Options &options)
    :C4ViewQueryEnumInternal(view),
     _reduce(options.reduce),
     _enum(view->_index, startKey, startKeyDocID, endKey, endKeyDocID, options)
    { }

    C4MapReduceEnumerator(C4View *view,
                        vector<KeyRange> keyRanges,
                        const IndexEnumerator::Options &options)
    :C4ViewQueryEnumInternal(view),
     _reduce(options.reduce),
     _enum(view->_index, keyRanges, options)
    { }

    virtual ~C4MapReduceEnumerator() {
        delete _reduce;
    }

    virtual bool next() override {
        if (!_enum.next())
            return C4ViewQueryEnumInternal::next();
        key = asKeyReader(_enum.key());
        value = _enum.value();
        docID = _enum.recordID();
        docSequence = _enum.sequence();
        return true;
    }

    virtual void close() noexcept override {
        _enum.close();
    }

private:
    ReduceFunction *_reduce {nullptr};
    IndexEnumerator _enum;
};


C4QueryEnumerator* c4view_query(C4View *view,
                                const C4QueryOptions *c4options,
                                C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        WITH_LOCK(view);
        if (!c4options)
            c4options = &kC4DefaultQueryOptions;
        IndexEnumerator::Options options = convertOptions(c4options);

        if (c4options->keysCount == 0 && c4options->keys == nullptr) {
            Collatable noKey;
            return new C4MapReduceEnumerator(view,
                                           (c4options->startKey ? (Collatable)*c4options->startKey
                                                                : noKey),
                                           c4options->startKeyDocID,
                                           (c4options->endKey ? (Collatable)*c4options->endKey
                                                              : noKey),
                                           c4options->endKeyDocID,
                                           options);
        } else {
            vector<KeyRange> keyRanges;
            for (size_t i = 0; i < c4options->keysCount; i++) {
                const C4Key* key = c4options->keys[i];
                if (key)
                    keyRanges.emplace_back(*key);
            }
            return new C4MapReduceEnumerator(view, keyRanges, options);
        }
    });
}


#pragma mark EXPRESSION-BASED QUERIES:


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
