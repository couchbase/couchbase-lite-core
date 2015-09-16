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
    c4Key()     :Collatable() { }
};


C4Key* c4key_new() {
    return new c4Key();
}

void c4key_free(C4Key *key) {
    delete key;
}

void c4Key_addNull(C4Key *key) {
    key->addNull();
}

void c4key_addBool(C4Key *key, bool b) {
    key->addBool(b);
}

void c4key_addNumber(C4Key *key, double n) {
    *key << n;
}

void c4key_addString(C4Key *key, C4Slice str) {
    *key << str;
}

void c4key_addMapKey(C4Key *key, C4Slice mapKey) {
    *key << mapKey;
}

void c4key_beginArray(C4Key *key) {
    key->beginArray();
}

void c4key_endArray(C4Key *key) {
    key->endArray();
}

void c4key_beginMap(C4Key *key) {
    key->beginMap();
}

void c4key_endMap(C4Key *key) {
    key->endMap();
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
    } catchError(outError);
    if (indexer)
        delete indexer;
    return NULL;
}


C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer, C4Error *outError) {
    try {
        sequence startSequence = indexer->startingSequence();
        if (startSequence == UINT64_MAX)
            return NULL;
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
                values.push_back(*emittedValues[i]);
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


const C4QueryOptions kC4DefaultQueryOptions = {
    .limit = UINT_MAX,
    .inclusiveStart = true,
    .inclusiveEnd = true
};
