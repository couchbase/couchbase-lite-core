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

#include "c4Index.h"
#include "c4Observer.h"
#include "c4Private.h"
#include "c4Query.h"

#include "c4ExceptionUtils.hh"
#include "c4Query.hh"
#include "c4QueryEnumeratorImpl.hh"
#include "c4QueryObserver.hh"

#include "SQLiteDataFile.hh"
#include "FleeceImpl.hh"


using namespace std;
using namespace litecore;
using namespace fleece::impl;

CBL_CORE_API const C4QueryOptions kC4DefaultQueryOptions = { };


#pragma mark - QUERY API:


C4Query* c4query_new2(C4Database *database,
                      C4QueryLanguage language,
                      C4Slice expression,
                      int *outErrorPos,
                      C4Error *outError) noexcept
{
    if (outErrorPos)
        *outErrorPos = -1;
    return tryCatch<C4Query*>(outError, [&]{
        try {
            return retain(new C4Query(database, language, expression));
        } catch (Query::parseError &x) {
            if (outErrorPos)
                *outErrorPos = x.errorPosition;
            throw;
        }
    });
}


C4Query* c4query_new(C4Database *database, C4String expression, C4Error *error) C4API {
    return c4query_new2(database, kC4JSONQuery, expression, nullptr, error);
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


void c4query_setParameters(C4Query *query, C4String encodedParameters) C4API {
    query->setParameters(encodedParameters);
}


C4QueryEnumerator* c4query_run(C4Query *query,
                               const C4QueryOptions *c4options,
                               C4Slice encodedParameters,
                               C4Error *outError) noexcept
{
    return tryCatch<C4QueryEnumerator*>(outError, [&]{
        return retain(query->createEnumerator(c4options, encodedParameters));
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


#pragma mark - QUERY ENUMERATOR API:


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


C4QueryEnumerator* c4queryenum_retain(C4QueryEnumerator *e) C4API {
    return retain(asInternal(e));
}


void c4queryenum_close(C4QueryEnumerator *e) noexcept {
    if (e) {
        asInternal(e)->close();
    }
}

void c4queryenum_release(C4QueryEnumerator *e) noexcept {
    release(asInternal(e));
}


#pragma mark - QUERY OBSERVER API:


C4QueryObserver* c4queryobs_create(C4Query *query, C4QueryObserverCallback cb, void *ctx) C4API {
    return new C4QueryObserver(query, cb, ctx);
}

void c4queryobs_setEnabled(C4QueryObserver *obs, bool enabled) C4API {
    obs->query()->enableObserver(obs, enabled);
}

void c4queryobs_free(C4QueryObserver* obs) C4API {
    if (obs) {
        obs->query()->enableObserver(obs, false);
        delete obs;
    }
}

C4QueryEnumerator* c4queryobs_getEnumerator(C4QueryObserver *obs,
                                            bool forget,
                                            C4Error *outError) C4API
{
    return retain(obs->currentEnumerator(forget, outError));
}


#pragma mark - INDEXES:


bool c4db_createIndex(C4Database *database,
                      C4Slice name,
                      C4Slice indexSpecJSON,
                      C4IndexType indexType,
                      const C4IndexOptions *indexOptions,
                      C4Error *outError) noexcept
{
    return c4db_createIndex2(database,
                             name,
                             indexSpecJSON,
                             kC4JSONQuery,
                             indexType,
                             indexOptions,
                             outError);
}


bool c4db_createIndex2(C4Database *database,
                       C4Slice name,
                       C4Slice indexSpec,
                       C4QueryLanguage queryLanguage,
                       C4IndexType indexType,
                       const C4IndexOptions *indexOptions,
                       C4Error *outError) noexcept
{
    static_assert(sizeof(C4IndexOptions) == sizeof(IndexSpec::Options),
                  "IndexSpec::Options types must match");
    return tryCatch(outError, [&]{
        database->defaultKeyStore().createIndex2(slice(name),
                                                 indexSpec,
                                                 (QueryLanguage)queryLanguage,
                                                 (IndexSpec::Type)indexType,
                                                 (const IndexSpec::Options*)indexOptions);
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
                enc.writeKey("expr"); enc.writeString(spec.expression);
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


C4SliceResult c4db_getIndexRows(C4Database* database, C4String indexName, C4Error* outError) noexcept {
    return tryCatch<C4SliceResult>(outError, [&]{
        int64_t rowCount;
        alloc_slice rows;
        ((SQLiteDataFile*)database->dataFile())->inspectIndex(indexName, rowCount, &rows);
        return C4SliceResult(rows);
    });
}
