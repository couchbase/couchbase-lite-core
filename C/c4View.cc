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

#include "c4ViewInternal.hh"
#include "c4DatabaseInternal.hh"
#include "c4DocInternal.hh"
#include "c4KeyInternal.hh"

#include "DataFile.hh"
#include "Tokenizer.hh"
#include <math.h>
#include <limits.h>
using namespace litecore;


#pragma mark - VIEWS:


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


bool c4key_setDefaultFullTextLanguage(C4Slice languageName, bool stripDiacriticals) {
    initTokenizer();
    Tokenizer::defaultStemmer = string(languageName);
    Tokenizer::defaultRemoveDiacritics = stripDiacriticals;
    return true;
}
