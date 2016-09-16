//
//  c4View.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
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
#include "c4Document.h"
#include "c4DocEnumerator.h"

#include "c4DatabaseInternal.hh"
#include "c4DocInternal.hh"
#include "c4KeyInternal.hh"

#include "DataFile.hh"
#include "Collatable.hh"
#include "MapReduceIndex.hh"
#include "FullTextIndex.hh"
#include "GeoIndex.hh"
#include "Tokenizer.hh"
#include <math.h>
#include <limits.h>
using namespace litecore;


// C4KeyReader is really identical to CollatableReader, which itself consists of nothing but
// a slice.
static inline C4KeyReader asKeyReader(const CollatableReader &r) {
    return *(C4KeyReader*)&r;
}


#pragma mark - VIEWS:


struct c4View : public RefCounted<c4View> {
    c4View(C4Database *sourceDB,
           const FilePath &path,
           C4Slice viewName,
           C4Slice version,
           const C4DatabaseConfig &config)
    :_sourceDB(sourceDB),
     _viewDB(c4Database::newDataFile(path, config, false)),
     _index(_viewDB->getKeyStore((string)viewName), *sourceDB->db())
    {
        setVersion(version);
    }

    void setVersion(C4Slice version) {
        _index.setup(-1, (string)version);
    }

    bool checkNotBusy(C4Error *outError) {
        if (_index.isBusy()) {
            recordError(LiteCoreDomain, kC4ErrorIndexBusy, outError);
            return false;
        }
        return true;
    }

    void close() {
        _viewDB->close();
    }

    Retained<C4Database> _sourceDB;
    unique_ptr<DataFile> _viewDB;
    MapReduceIndex _index;
#if C4DB_THREADSAFE
    mutex _mutex;
#endif
};


static FilePath pathForViewNamed(C4Database *db, C4Slice viewName) {
    FilePath dbPath = db->db()->filePath();
    string quotedName = FilePath::sanitizedFileName((string)viewName);
    return dbPath.fileNamed(quotedName).addingExtension("viewindex");
}


C4View* c4view_open(C4Database* db,
                    C4Slice pathSlice,
                    C4Slice viewName,
                    C4Slice version,
                    const C4DatabaseConfig *config,
                    C4Error *outError)
{
    if (!checkParam(config != nullptr, outError))
        return nullptr;
    try {
        FilePath path = (pathSlice.buf) ? FilePath((string)pathSlice)
                                        : pathForViewNamed(db, viewName);
        return (new c4View(db, path, viewName, version, *config))->retain();
    } catchError(outError);
    return NULL;
}

/** Closes the view and frees the object. */
bool c4view_close(C4View* view, C4Error *outError) {
    if (!view)
        return true;
    try {
        WITH_LOCK(view);
        if (!view->checkNotBusy(outError))
            return false;
        view->close();
        return true;
    } catchError(outError);
    return false;
}

void c4view_free(C4View* view) {
    if (view) {
        c4view_close(view, NULL);
        try {
            view->release();
        } catchExceptions();
    }
}


bool c4view_rekey(C4View *view, const C4EncryptionKey *newKey, C4Error *outError) {
    WITH_LOCK(view);
    if (!view->checkNotBusy(outError))
        return false;
    return c4Database::rekey(view->_viewDB.get(), newKey, outError);
}

bool c4view_eraseIndex(C4View *view, C4Error *outError) {
    try {
        WITH_LOCK(view);
        view->_index.erase();
        return true;
    } catchError(outError);
    return false;
}

bool c4view_delete(C4View *view, C4Error *outError) {
    try {
        if (view == NULL) {
            return true;
        }

        WITH_LOCK(view);
        if (!view->checkNotBusy(outError))
            return false;
        view->_viewDB->deleteDataFile();
        view->close();
        return true;
    } catchError(outError)
    return false;
}

bool c4view_deleteAtPath(C4Slice viewPath, const C4DatabaseConfig *config, C4Error *outError) {
    if (!checkParam(config != nullptr, outError))
        return false;
    return c4db_deleteAtPath(viewPath, config, outError);
}


bool c4view_deleteByName(C4Database *database, C4Slice viewName, C4Error *outError) {
    FilePath path = pathForViewNamed(database, viewName);
    return c4view_deleteAtPath((slice)path.path(), &database->config, outError);
}


void c4view_setMapVersion(C4View *view, C4Slice version) {
    try {
        WITH_LOCK(view);
        view->setVersion(version);
    } catchExceptions();
}


uint64_t c4view_getTotalRows(C4View *view) {
    try {
        WITH_LOCK(view);
        return view->_index.rowCount();
    } catchExceptions();
    return 0;
}

C4SequenceNumber c4view_getLastSequenceIndexed(C4View *view) {
    try {
        WITH_LOCK(view);
        return view->_index.lastSequenceIndexed();
    } catchExceptions();
    return 0;
}

C4SequenceNumber c4view_getLastSequenceChangedAt(C4View *view) {
    try {
        WITH_LOCK(view);
        return view->_index.lastSequenceChangedAt();
    } catchExceptions();
    return 0;
}


void c4view_setDocumentType(C4View *view, C4Slice docType) {
    WITH_LOCK(view);
    view->_index.setDocumentType(docType);
}


void c4view_setOnCompactCallback(C4View *view, C4OnCompactCallback cb, void *context) {
    WITH_LOCK(view);
    view->_viewDB->setOnCompact([cb,context](bool compacting) {
        cb(context, compacting);
    });
}


#pragma mark - INDEXING:


static void initTokenizer() {
    static bool sInitializedTokenizer = false;
    if (!sInitializedTokenizer) {
        Tokenizer::defaultStemmer = "english";
        Tokenizer::defaultRemoveDiacritics = true;
        sInitializedTokenizer = true;
    }
}


struct c4Indexer : public MapReduceIndexer, InstanceCounted {
    c4Indexer(C4Database *db)
    :MapReduceIndexer(),
     _db(db)
    {
        initTokenizer();
    }

    virtual ~c4Indexer() {
#if C4DB_THREADSAFE
        for (auto view : _views)
            view->_mutex.unlock();
#endif
    }

    void addView(C4View *view) {
#if C4DB_THREADSAFE
        view->_mutex.lock();
        _views.push_back(view);
#endif
        WITH_LOCK(view->_sourceDB); // MapReduceIndexer::addIndex ends up calling _sourceDB
        addIndex(view->_index);
    }

    void finished() {
        MapReduceIndexer::finished(_lastSequenceIndexed);
    }

    C4Database* _db;
    sequence _lastSequenceIndexed {0};
#if C4DB_THREADSAFE
    vector<C4View*> _views;
#endif
};


C4Indexer* c4indexer_begin(C4Database *db,
                           C4View *views[],
                           size_t viewCount,
                           C4Error *outError)
{
    c4Indexer *indexer = NULL;
    try {
        indexer = new c4Indexer(db);
        for (size_t i = 0; i < viewCount; ++i)
            indexer->addView(views[i]);
        return indexer;
    } catchError(outError);
    if (indexer)
        delete indexer;
    return NULL;
}


void c4indexer_triggerOnView(C4Indexer *indexer, C4View *view) {
    indexer->triggerOnIndex(&view->_index);
}


C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer, C4Error *outError) {
    try {
        sequence startSequence;
        {
            WITH_LOCK(indexer->_db);       // startingSequence calls _sourceDB
            startSequence = indexer->startingSequence();
        }
        if (startSequence == UINT64_MAX) {
            clearError(outError);      // end of iteration is not an error
            return NULL;
        }

        auto options = kC4DefaultEnumeratorOptions;
        options.flags |= kC4IncludeDeleted | kC4IncludePurged;
        auto docTypes = indexer->documentTypes();
        if (docTypes)
            options.flags &= ~kC4IncludeBodies;
        auto e = c4db_enumerateChanges(indexer->_db, startSequence-1, &options, outError);
        if (!e)
            return NULL;

        setEnumFilter(e, [docTypes,indexer](const Document &doc,
                                            C4DocumentFlags flags,
                                            slice docType) {
            indexer->_lastSequenceIndexed = doc.sequence();
            if ((flags & kExists) && !(flags & kDeleted)
                                  && (!docTypes || docTypes->count(docType) > 0))
                return true;
            // We're skipping this doc because it's either purged or deleted, or its docType
            // doesn't match. But we do have to update the index to _remove_ it
            indexer->skipDoc(doc.key(), doc.sequence());
            return false;
        });
        return e;
    } catchError(outError);
    return NULL;
}


bool c4indexer_shouldIndexDocument(C4Indexer *indexer,
                                   unsigned viewNumber,
                                   C4Document *doc)
{
    try {
        auto idoc = c4Internal::internal(doc);
        if (!indexer->shouldMapDocIntoView(idoc->document(), viewNumber))
            return false;
        else if (indexer->shouldMapDocTypeIntoView(idoc->type(), viewNumber))
            return true;
        else {
            // We're skipping this doc, but we do have to update the index to _remove_ it
            indexer->skipDocInView(idoc->document().key(), idoc->sequence, viewNumber);
            return false;
        }
    } catchExceptions()
    return true;
}


bool c4indexer_emit(C4Indexer *indexer,
                    C4Document *doc,
                    unsigned viewNumber,
                    unsigned emitCount,
                    C4Key* const emittedKeys[],
                    C4Slice const emittedValues[],
                    C4Error *outError)
{
    C4KeyValueList kv;
    kv.keys.reserve(emitCount);
    kv.values.reserve(emitCount);
    for (unsigned i = 0; i < emitCount; ++i) {
        c4kv_add(&kv, emittedKeys[i], emittedValues[i]);
    }
    return c4indexer_emitList(indexer, doc, viewNumber, &kv, outError);
}


bool c4indexer_emitList(C4Indexer *indexer,
                    C4Document *doc,
                    unsigned viewNumber,
                    C4KeyValueList *kv,
                    C4Error *outError)
{
    try {
        if (doc->flags & kDeleted)
            c4kv_reset(kv);
        indexer->emitDocIntoView(doc->docID, doc->sequence, viewNumber, kv->keys, kv->values);
        return true;
    } catchError(outError)
    return false;
}


bool c4indexer_end(C4Indexer *indexer, bool commit, C4Error *outError) {
    try {
        if (commit)
            indexer->finished();
        delete indexer;
        return true;
    } catchError(outError)
    return false;
}


#pragma mark - QUERIES:


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


bool c4key_setDefaultFullTextLanguage(C4Slice languageName, bool stripDiacriticals) {
    initTokenizer();
    Tokenizer::defaultStemmer = string(languageName);
    Tokenizer::defaultRemoveDiacritics = stripDiacriticals;
    return true;
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
