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

#include "c4ViewInternal.hh"
#include "c4DatabaseInternal.hh"
#include "c4KeyInternal.hh"

#include "DataFile.hh"
#include "Collatable.hh"
#include "FullTextIndex.hh"
#include "GeoIndex.hh"
#include "Tokenizer.hh"
#include <math.h>
#include <limits.h>
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

static DocEnumerator::Options convertOptions(const C4QueryOptions *c4options) {
    if (!c4options)
        c4options = &kC4DefaultQueryOptions;
    DocEnumerator::Options options = DocEnumerator::Options::kDefault;
    options.skip = (unsigned)c4options->skip;
    options.limit = (unsigned)c4options->limit;
    options.descending = c4options->descending;
    options.inclusiveStart = c4options->inclusiveStart;
    options.inclusiveEnd = c4options->inclusiveEnd;
    return options;
}


struct C4QueryEnumInternal : public C4QueryEnumerator, InstanceCounted {
    C4QueryEnumInternal(C4View *view)
    :_view(view)
#if C4DB_THREADSAFE
     ,_mutex(view->_mutex)
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

    Retained<C4View> _view;
#if C4DB_THREADSAFE
    mutex &_mutex;
#endif
};

static C4QueryEnumInternal* asInternal(C4QueryEnumerator *e) {return (C4QueryEnumInternal*)e;}


bool c4queryenum_next(C4QueryEnumerator *e,
                      C4Error *outError)
{
    try {
        WITH_LOCK(asInternal(e));
        if (asInternal(e)->next())
            return true;
        clearError(outError);      // end of iteration is not an error
    } catchError(outError);
    return false;
}


void c4queryenum_close(C4QueryEnumerator *e) {
    if (e) {
        WITH_LOCK(asInternal(e));
        asInternal(e)->close();
    }
}

void c4queryenum_free(C4QueryEnumerator *e) {
    c4queryenum_close(e);
    delete asInternal(e);
}


#pragma mark MAP/REDUCE QUERIES:


struct C4MapReduceEnumerator : public C4QueryEnumInternal {
    C4MapReduceEnumerator(C4View *view,
                        Collatable startKey, slice startKeyDocID,
                        Collatable endKey, slice endKeyDocID,
                        const DocEnumerator::Options &options)
    :C4QueryEnumInternal(view),
     _enum(view->_index, startKey, startKeyDocID, endKey, endKeyDocID, options)
    { }

    C4MapReduceEnumerator(C4View *view,
                        vector<KeyRange> keyRanges,
                        const DocEnumerator::Options &options)
    :C4QueryEnumInternal(view),
     _enum(view->_index, keyRanges, options)
    { }

    virtual bool next() override {
        if (!_enum.next())
            return C4QueryEnumInternal::next();
        key = asKeyReader(_enum.key());
        value = _enum.value();
        docID = _enum.docID();
        docSequence = _enum.sequence();
        return true;
    }

    virtual void close() noexcept override {
        _enum.close();
    }

private:
    IndexEnumerator _enum;
};


C4QueryEnumerator* c4view_query(C4View *view,
                                const C4QueryOptions *c4options,
                                C4Error *outError)
{
    try {
        WITH_LOCK(view);
        if (!c4options)
            c4options = &kC4DefaultQueryOptions;
        DocEnumerator::Options options = convertOptions(c4options);

        if (c4options->keysCount == 0 && c4options->keys == NULL) {
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
                    keyRanges.push_back(KeyRange(*key));
            }
            return new C4MapReduceEnumerator(view, keyRanges, options);
        }
    } catchError(outError);
    return NULL;
}


#pragma mark FULL-TEXT QUERIES:


struct C4FullTextEnumerator : public C4QueryEnumInternal {
    C4FullTextEnumerator(C4View *view,
                         slice queryString,
                         slice queryStringLanguage,
                         bool ranked,
                         const DocEnumerator::Options &options)
    :C4QueryEnumInternal(view),
     _enum(view->_index, queryString, queryStringLanguage, ranked, options)
    { }

    virtual bool next() override {
        if (!_enum.next())
            return C4QueryEnumInternal::next();
        auto match = _enum.match();
        docID = match->docID;
        docSequence = match->sequence;
        _allocatedValue = match->value();
        value = _allocatedValue;
        fullTextID = match->fullTextID();
        fullTextTermCount = (uint32_t)match->textMatches.size();
        fullTextTerms = (const C4FullTextTerm*)match->textMatches.data();
        return true;
    }

    alloc_slice fullTextMatched() {
        return _enum.match()->matchedText();
    }

    virtual void close() noexcept override {
        _enum.close();
    }

private:
    FullTextIndexEnumerator _enum;
    alloc_slice _allocatedValue;
};


C4QueryEnumerator* c4view_fullTextQuery(C4View *view,
                                        C4Slice queryString,
                                        C4Slice queryStringLanguage,
                                        const C4QueryOptions *c4options,
                                        C4Error *outError)
{
    try {
        WITH_LOCK(view);
        if (queryStringLanguage == kC4LanguageDefault)
            queryStringLanguage = Tokenizer::defaultStemmer;
        return new C4FullTextEnumerator(view, queryString, queryStringLanguage,
                                        (c4options ? c4options->rankFullText : true),
                                        convertOptions(c4options));
    } catchError(outError);
    return NULL;
}


C4SliceResult c4view_fullTextMatched(C4View *view,
                                     C4Slice docID,
                                     C4SequenceNumber seq,
                                     unsigned fullTextID,
                                     C4Error *outError)
{
    try {
        WITH_LOCK(view);
        auto result = FullTextMatch::matchedText(&view->_index, docID, seq, fullTextID).dontFree();
        return {result.buf, result.size};
    } catchError(outError);
    return {NULL, 0};
}


C4SliceResult c4queryenum_fullTextMatched(C4QueryEnumerator *e) {
    try {
        slice result = ((C4FullTextEnumerator*)e)->fullTextMatched().dontFree();
        return {result.buf, result.size};
    } catchExceptions();
    return {NULL, 0};
}


#pragma mark GEO-QUERIES:


struct C4GeoEnumerator : public C4QueryEnumInternal {
    C4GeoEnumerator(C4View *view, const geohash::area &bbox)
    :C4QueryEnumInternal(view),
     _enum(view->_index, bbox)
    { }

    virtual bool next() override {
        if (!_enum.next())
            return C4QueryEnumInternal::next();
        docID = _enum.docID();
        docSequence = _enum.sequence();
        value = _enum.value();
        auto bbox = _enum.keyBoundingBox();
        geoBBox.xmin = bbox.min().longitude;
        geoBBox.ymin = bbox.min().latitude;
        geoBBox.xmax = bbox.max().longitude;
        geoBBox.ymax = bbox.max().latitude;
        geoJSON = _enum.keyGeoJSON();
        return true;
    }

    virtual void close() noexcept override {
        _enum.close();
    }

private:
    GeoIndexEnumerator _enum;
};


C4QueryEnumerator* c4view_geoQuery(C4View *view,
                                   C4GeoArea area,
                                   C4Error *outError)
{
    try {
        WITH_LOCK(view);
        geohash::area ga(geohash::coord(area.ymin, area.xmin),
                         geohash::coord(area.ymax, area.xmax));
        return new C4GeoEnumerator(view, ga);
    } catchError(outError);
    return NULL;
}
