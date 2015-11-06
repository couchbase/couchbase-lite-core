//
//  c4View.cc
//  CBForest
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4View.h"
#include "c4Document.h"
#include "Collatable.hh"
#include "MapReduceIndex.hh"
#include <math.h>
#include <limits.h>
using namespace forestdb;


// ForestDB Write-Ahead Log size (# of records)
static const size_t kViewDBWALThreshold = 1024;


// C4KeyReader is really identical to CollatableReader, which itself consists of nothing but
// a slice.
static inline C4KeyReader asKeyReader(const CollatableReader &r) {
    return *(C4KeyReader*)&r;
}


#pragma mark - VIEWS:


struct c4View {
    c4View(C4Database *sourceDB,
           C4Slice path,
           C4Slice name,
           const Database::config &config,
           C4Slice version)
    :_sourceDB(sourceDB),
     _viewDB((std::string)path, config),
     _index(&_viewDB, (std::string)name, asDatabase(sourceDB)->defaultKeyStore())
    {
        Transaction t(&_viewDB);
        _index.setup(t, -1, NULL, (std::string)version);
    }

    C4Database *_sourceDB;
    Database _viewDB;
    MapReduceIndex _index;
};


C4View* c4view_open(C4Database* db,
                    C4Slice path,
                    C4Slice viewName,
                    C4Slice version,
                    C4DatabaseFlags flags,
                    const C4EncryptionKey *key,
                    C4Error *outError)
{
    try {
        auto config = c4DbConfig(flags, key);
        config.wal_threshold = kViewDBWALThreshold;
        config.seqtree_opt = FDB_SEQTREE_NOT_USE; // indexes don't need by-sequence ordering

        return new c4View(db, path, viewName, config, version);
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

bool c4view_rekey(C4View *view, const C4EncryptionKey *newKey, C4Error *outError) {
    return rekey(&view->_viewDB, newKey, outError);
}

bool c4view_eraseIndex(C4View *view, C4Error *outError) {
    try {
        Transaction t(&view->_viewDB);
        view->_index.erase(t);
        return true;
    } catchError(outError);
    return false;
}

bool c4view_delete(C4View *view, C4Error *outError) {
    try {
		if (view == NULL) {
			return true;
		}

        view->_viewDB.deleteDatabase();
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
                           size_t viewCount,
                           C4Error *outError)
{
    c4Indexer *indexer = NULL;
    try {
        indexer = new c4Indexer(db);
        for (size_t i = 0; i < viewCount; ++i) {
            auto t = new Transaction(&views[i]->_viewDB);
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
        auto options = kC4DefaultEnumeratorOptions;
        options.flags |= kC4IncludeDeleted;
        return c4db_enumerateChanges(indexer->_db, startSequence-1, &options, outError);
    } catchError(outError);
    return NULL;
}

bool c4indexer_emit(C4Indexer *indexer,
                    C4Document *doc,
                    unsigned viewIndex,
                    unsigned emitCount,
                    C4Key* emittedKeys[],
                    C4Slice emittedValues[],
                    C4Error *outError)
{
    try {
        std::vector<Collatable> keys;
        std::vector<slice> values;
        if (!(doc->flags & kDeleted)) {
            for (unsigned i = 0; i < emitCount; ++i) {
                keys.push_back(*emittedKeys[i]);
                values.push_back(emittedValues[i]);
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


CBFOREST_API const C4QueryOptions kC4DefaultQueryOptions = {
	0,
    UINT_MAX,
	false,
    true,
    true
};


C4QueryEnumerator* c4view_query(C4View *view,
                                const C4QueryOptions *c4options,
                                C4Error *outError)
{
    try {
        if (!c4options)
            c4options = &kC4DefaultQueryOptions;
        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.skip = (unsigned)c4options->skip;
        options.limit = (unsigned)c4options->limit;
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
            for (size_t i = 0; i < c4options->keysCount; i++) {
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
            ei->value = ei->_enum.value();
            ei->docID = ei->_enum.docID();
            ei->docSequence = ei->_enum.sequence();
            return true;
        } else {
            ei->key = {NULL, 0};
            ei->value = slice::null;
            ei->docID = slice::null;
            ei->docSequence = 0;
            recordError(FDB_RESULT_SUCCESS, outError);      // end of iteration is not an error
            return false;
        }
    } catchError(outError);
    return false;
}


void c4queryenum_free(C4QueryEnumerator *e) {
    delete asInternal(e);
}
