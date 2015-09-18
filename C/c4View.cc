//
//  c4View.cc
//  CBForest
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4View.h"
#include "Collatable.hh"
#include "MapReduceIndex.hh"

using namespace forestdb;


// Size of ForestDB buffer cache allocated for a database
static const size_t kViewDBBufferCacheSize = (8*1024*1024);

// ForestDB Write-Ahead Log size (# of records)
static const size_t kViewDBWALThreshold = 1024;


#pragma mark - KEYS:


struct c4Key : public Collatable {
    c4Key()                 :Collatable() { }
    c4Key(C4Slice bytes)    :Collatable(bytes, true) { }
};

C4Key* c4key_new()                              {return new c4Key();}
C4Key* c4key_withBytes(C4Slice bytes)           {return new c4Key(bytes);}
void c4key_free(C4Key *key)                     {delete key;}
void c4key_addNull(C4Key *key)                  {key->addNull();}
void c4key_addBool(C4Key *key, bool b)          {key->addBool(b);}
void c4key_addNumber(C4Key *key, double n)      {*key << n;}
void c4key_addString(C4Key *key, C4Slice str)   {*key << str;}
void c4key_addMapKey(C4Key *key, C4Slice mapKey){*key << mapKey;}
void c4key_beginArray(C4Key *key)               {key->beginArray();}
void c4key_endArray(C4Key *key)                 {key->endArray();}
void c4key_beginMap(C4Key *key)                 {key->beginMap();}
void c4key_endMap(C4Key *key)                   {key->endMap();}

// C4KeyReader is really identical to CollatableReader, which itself consists of nothing but
// a slice. So these functions use pointer-casting to reinterpret C4KeyReader as CollatableReader.

static inline C4KeyReader asKeyReader(const CollatableReader &r) {
    return *(C4KeyReader*)&r;
}


C4KeyReader c4key_read(C4Key *key) {
    CollatableReader r(*key);
    return asKeyReader(r);
}


C4KeyToken c4key_peek(C4KeyReader* r) {
    static const C4KeyToken tagToType[] = {kC4EndSequence, kC4Null, kC4Bool, kC4Bool, kC4Number,
                                    kC4Number, kC4String, kC4Array, kC4Map, kC4Error, kC4Special};
    Collatable::Tag t = ((CollatableReader*)r)->peekTag();
    if (t >= sizeof(tagToType)/sizeof(tagToType[0]))
        return kC4Error;
    return tagToType[t];
}

void c4key_skipToken(C4KeyReader* r) {
    ((CollatableReader*)r)->skipTag();
}

bool c4key_readBool(C4KeyReader* r) {
    bool result = false;
    try {
        result = ((CollatableReader*)r)->peekTag() >= CollatableReader::kTrue;
        ((CollatableReader*)r)->skipTag();
    } catchError(NULL)
    return result;
}

double c4key_readNumber(C4KeyReader* r) {
    try {
    return ((CollatableReader*)r)->readDouble();
    } catchError(NULL)
    return nan("err");  // ????
}

C4SliceResult c4key_readString(C4KeyReader* r) {
    slice s;
    try {
        s = ((CollatableReader*)r)->readString().copy();
        //OPT: This makes an extra copy because I can't find a way to 'adopt' the allocated block
        // managed by the alloc_slice's std::shared_ptr.
    } catchError(NULL)
    return (C4SliceResult){s.buf, s.size};
}

C4SliceResult c4key_toJSON(const C4KeyReader* r) {
    if (!r || r->length == 0)
        return (C4SliceResult){NULL, 0};
    std::string str = ((CollatableReader*)r)->toJSON();
    auto s = ((slice)str).copy();
    return (C4SliceResult){s.buf, s.size};
}


#pragma mark - VIEWS:


struct c4View {
    c4View(C4Database *sourceDB,
           Database *viewDB,
           C4Slice name,
           C4Slice version)
    :_sourceDB(sourceDB),
     _viewDB(viewDB),
     _index(viewDB, (std::string)name, internal(sourceDB)->defaultKeyStore()),
     _version(version)
    { }

    C4Database *_sourceDB;
    Database *_viewDB;
    MapReduceIndex _index;
    std::string _version;
};


C4View* c4view_open(C4Database* db,
                    C4Slice path,
                    C4Slice viewName,
                    C4Slice version,
                    C4Error *outError)
{
    try {
        auto config = Database::defaultConfig();
        config.flags = FDB_OPEN_FLAG_CREATE;
        config.buffercache_size = kViewDBBufferCacheSize;
        config.wal_threshold = kViewDBWALThreshold;
        config.wal_flush_before_commit = true;
        config.seqtree_opt = FDB_SEQTREE_NOT_USE; // indexes don't need by-sequence ordering
        config.compaction_threshold = 50;

        auto viewDB = new Database((std::string)path, config);
        return new c4View(db, viewDB, viewName, version);
    } catchError(outError);
    return NULL;
}

/** Closes the view and frees the object. */
bool c4view_close(C4View* view, C4Error *outError) {
    try {
        delete view;
        return true;
    } catchError(outError);
    return false;
}

bool c4view_eraseIndex(C4View *view, C4Error *outError) {
    try {
        Transaction t(view->_viewDB);
        view->_index.erase(t);
        return true;
    } catchError(outError);
    return false;
}

bool c4view_delete(C4View *view, C4Error *outError) {
    try {
        view->_viewDB->deleteDatabase();
        delete view;
        return true;
    } catchError(outError)
    return false;
}


uint64_t c4view_getTotalRows(C4View *view) {
    try {
        return view->_index.rowCount();
    } catchError(NULL);
    return 0;
}

C4SequenceNumber c4view_getLastSequenceIndexed(C4View *view) {
    try {
        return view->_index.lastSequenceIndexed();
    } catchError(NULL);
    return 0;
}

C4SequenceNumber c4view_getLastSequenceChangedAt(C4View *view) {
    try {
        return view->_index.lastSequenceChangedAt();
    } catchError(NULL);
    return 0;
}


#pragma mark - INDEXING:


struct c4Indexer : public MapReduceIndexer {
    c4Indexer(C4Database *db)
    :MapReduceIndexer(),
     _db(db)
    { }

    virtual ~c4Indexer() { }

    C4Database* _db;
};


C4Indexer* c4indexer_begin(C4Database *db,
                           C4View *views[],
                           int viewCount,
                           C4Error *outError)
{
    c4Indexer *indexer = NULL;
    try {
        indexer = new c4Indexer(db);
        for (int i = 0; i < viewCount; ++i) {
            auto t = new Transaction(views[i]->_viewDB);
            indexer->addIndex(&views[i]->_index, t);
        }
        return indexer;
    } catchError(outError);
    if (indexer)
        delete indexer;
    return NULL;
}


C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer, C4Error *outError) {
    try {
        sequence startSequence = indexer->startingSequence();
        if (startSequence == UINT64_MAX) {
            recordError(FDB_RESULT_SUCCESS, outError);      // end of iteration is not an error
            return NULL;
        }
        auto options = kC4DefaultChangesOptions;
        options.includeDeleted = true;
        return c4db_enumerateChanges(indexer->_db, startSequence-1, &options, outError);
    } catchError(outError);
    return NULL;
}

bool c4indexer_emit(C4Indexer *indexer,
                    C4Document *doc,
                    unsigned viewIndex,
                    unsigned emitCount,
                    C4Key* emittedKeys[],
                    C4Key* emittedValues[],
                    C4Error *outError)
{
    try {
        std::vector<Collatable> keys, values;
        if (!(doc->flags & kDeleted)) {
            for (unsigned i = 0; i < emitCount; ++i) {
                keys.push_back(*emittedKeys[i]);
                values.push_back(emittedValues[i] ? *emittedValues[i] : Collatable());
            }
        }
        indexer->emitDocIntoView(doc->docID, doc->sequence, viewIndex, keys, values);
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


struct C4QueryEnumInternal : public C4QueryEnumerator {
    C4QueryEnumInternal(C4View *view,
                        Collatable startKey, slice startKeyDocID,
                        Collatable endKey, slice endKeyDocID,
                        const DocEnumerator::Options &options)
    :_enum(&view->_index, startKey, startKeyDocID, endKey, endKeyDocID, options)
    { }

    C4QueryEnumInternal(C4View *view,
                        std::vector<KeyRange> keyRanges,
                        const DocEnumerator::Options &options)
    :_enum(&view->_index, keyRanges, options)
    { }

    IndexEnumerator _enum;
};

static C4QueryEnumInternal* asInternal(C4QueryEnumerator *e) {return (C4QueryEnumInternal*)e;}


const C4QueryOptions kC4DefaultQueryOptions = {
    .limit = UINT_MAX,
    .inclusiveStart = true,
    .inclusiveEnd = true
};


C4QueryEnumerator* c4view_query(C4View *view,
                                const C4QueryOptions *c4options,
                                C4Error *outError)
{
    try {
        if (!c4options)
            c4options = &kC4DefaultQueryOptions;
        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.skip = c4options->skip;
        options.limit = c4options->limit;
        options.descending = c4options->descending;
        options.inclusiveStart = c4options->inclusiveStart;
        options.inclusiveEnd = c4options->inclusiveEnd;

        if (c4options->keysCount == 0 && c4options->keys == NULL) {
            Collatable noKey;
            return new C4QueryEnumInternal(view,
                                           (c4options->startKey ? *c4options->startKey : noKey),
                                           c4options->startKeyDocID,
                                           (c4options->endKey ? *c4options->endKey : noKey),
                                           c4options->endKeyDocID,
                                           options);
        } else {
            std::vector<KeyRange> keyRanges;
            for (int i = 0; i < c4options->keysCount; i++) {
                const C4Key* key = c4options->keys[i];
                if (key)
                    keyRanges.push_back(KeyRange(*key));
            }
            return new C4QueryEnumInternal(view, keyRanges, options);
        }
    } catchError(outError);
    return NULL;
}


bool c4queryenum_next(C4QueryEnumerator *e,
                      C4Error *outError)
{
    try {
        auto ei = asInternal(e);
        if (ei->_enum.next()) {
            ei->key = asKeyReader(ei->_enum.key());
            ei->value = asKeyReader(ei->_enum.value());
            ei->docID = ei->_enum.docID();
            return true;
        } else {
            ei->key = ei->value = (C4KeyReader){NULL, 0};
            ei->docID = slice::null;
            recordError(FDB_RESULT_SUCCESS, outError);      // end of iteration is not an error
            return false;
        }
    } catchError(outError);
    return false;
}


void c4queryenum_free(C4QueryEnumerator *e) {
    delete asInternal(e);
}
