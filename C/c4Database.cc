//
//  c4Database.cc
//  CBForest
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "slice.hh"
typedef forestdb::slice C4Slice;
typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;

#define kC4SliceNull forestdb::slice::null

#define C4_IMPL
#include "c4Database.h"
#undef C4_IMPL

#include "Database.hh"
#include "Document.hh"
#include "DocEnumerator.hh"
#include "LogInternal.hh"
#include "VersionedDocument.hh"
#include <assert.h>

using namespace forestdb;


// Size of ForestDB buffer cache allocated for a database
#define kDBBufferCacheSize (8*1024*1024)

// ForestDB Write-Ahead Log size (# of records)
#define kDBWALThreshold 1024

// How often ForestDB should check whether databases need auto-compaction
#define kAutoCompactInterval (5*60.0)


static void recordHTTPError(int httpStatus, C4Error* outError) {
    if (outError) {
        outError->domain = C4Error::HTTPDomain;
        outError->code = httpStatus;
    }
}

static void recordError(error e, C4Error* outError) {
    if (outError) {
        outError->domain = C4Error::ForestDBDomain;
        outError->code = e.status;
    }
}

static void recordUnknownException(C4Error* outError) {
    Warn("Unexpected C++ exception thrown from CBForest");
    if (outError) {
        outError->domain = C4Error::C4Domain;
        outError->code = 2;
    }
}


#define catchError(OUTERR) \
    catch (error err) { \
        recordError(err, OUTERR); \
    } catch (...) { \
        recordUnknownException(OUTERR); \
    }


void c4slice_free(C4Slice slice) {
    slice.free();
}


#pragma mark - DATABASES:


struct c4Database {
    Database* _db;

    c4Database()    :_db(NULL), _transaction(NULL), _transactionLevel(0) { }
    ~c4Database()   {assert(_transactionLevel == 0); delete _db;}

    void beginTransaction() {
        if (++_transactionLevel == 1)
            _transaction = new Transaction(_db);
    }

    Transaction* transaction() {
        assert(_transaction);
        return _transaction;
    }

    bool inTransaction() { return _transactionLevel > 0; }

    void endTransaction(bool commit = true) {
        assert(_transactionLevel > 0);
        if (--_transactionLevel == 0) {
            auto t = _transaction;
            _transaction = NULL;
            if (!commit)
                t->abort();
            delete t; // this commits/aborts the transaction
        }
    }

private:
    Transaction* _transaction;
    int _transactionLevel;
};


C4Database* c4db_open(C4Slice path,
                      bool readOnly,
                      C4Error *outError)
{
    auto config = Database::defaultConfig();
    config.flags = readOnly ? FDB_OPEN_FLAG_RDONLY : FDB_OPEN_FLAG_CREATE;
    config.buffercache_size = kDBBufferCacheSize;
    config.wal_threshold = kDBWALThreshold;
    config.wal_flush_before_commit = true;
    config.seqtree_opt = true;
    config.compress_document_body = true;
    config.compactor_sleep_duration = (uint64_t)kAutoCompactInterval;

    auto c4db = new c4Database;
    try {
        c4db->_db = new Database((std::string)path, config);
        return c4db;
    } catchError(outError);
    delete c4db;
    return NULL;
}


void c4db_close(C4Database* database) {
    delete database;
}


uint64_t c4db_getDocumentCount(C4Database* database) {
    try {
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = Database::kMetaOnly;

        uint64_t count = 0;
        for (DocEnumerator e(*database->_db, forestdb::slice::null, forestdb::slice::null, opts);
                e.next(); ) {
            VersionedDocument vdoc(*database->_db, *e);
            if (!vdoc.isDeleted())
                ++count;
        }
        return count;
    } catchError(NULL);
    return 0;
}


C4SequenceNumber c4db_getLastSequence(C4Database* database) {
    return database->_db->lastSequence();
}


bool c4db_isInTransaction(C4Database* database) {
    return database->inTransaction();
}

bool c4db_beginTransaction(C4Database* database,
                           C4Error *outError)
{
    try {
        database->beginTransaction();
        return true;
    } catchError(outError);
    return false;
}

bool c4db_endTransaction(C4Database* database,
                         bool commit,
                         C4Error *outError)
{
    try {
        database->endTransaction(commit);
        return true;
    } catchError(outError);
    return false;
}


#pragma mark - RAW DOCUMENTS:


void c4raw_free(C4RawDocument* rawDoc) {
    if (rawDoc) {
        c4slice_free(rawDoc->key);
        c4slice_free(rawDoc->meta);
        c4slice_free(rawDoc->body);
        delete rawDoc;
    }
}


C4RawDocument* c4raw_get(C4Database* database,
                         C4Slice storeName,
                         C4Slice key,
                         C4Error *outError)
{
    try {
        KeyStore localDocs(database->_db, (std::string)storeName);
        Document doc = localDocs.get(key);
        if (!doc.exists()) {
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
            return NULL;
        }
        auto rawDoc = new C4RawDocument;
        rawDoc->key = doc.key().copy();
        rawDoc->meta = doc.meta().copy();
        rawDoc->body = doc.body().copy();
        return rawDoc;
    } catchError(outError);
    return NULL;
}


bool c4raw_put(C4Database* database,
               C4Slice storeName,
               C4Slice key,
               C4Slice meta,
               C4Slice body,
               C4Error *outError)
{
    bool abort = false;
    try {
        database->beginTransaction();
        abort = true;
        KeyStore localDocs(database->_db, (std::string)storeName);
        KeyStoreWriter localWriter = (*database->transaction())(localDocs);
        if (body.buf || meta.buf)
            localWriter.set(key, meta, body);
        else
            localWriter.del(key);
        abort = false;
        database->endTransaction();
    } catchError(outError);
    if (abort)
        database->endTransaction(false);
    return false;
}


#pragma mark - DOCUMENTS:


struct C4Document {
    C4Database* _db;
    VersionedDocument _versionedDoc;

    C4Document(C4Database* database, C4Slice docID)
    :_db(database),
     _versionedDoc(*_db->_db, docID)
    { }

    C4Document(C4Database *database, const Document &doc)
    :_db(database),
     _versionedDoc(*_db->_db, doc)
    { }
};


void c4doc_free(C4Document *doc) {
    delete doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        auto doc = new C4Document(database, docID);
        if (mustExist && !doc->_versionedDoc.exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


C4SliceResult c4doc_getRevID(C4Document *doc) {
    slice result = doc->_versionedDoc.revID().expanded().copy();
    return (C4SliceResult){result.buf, result.size};
}

C4DocumentFlags c4doc_getFlags(C4Document *doc) {
    auto flags = (C4DocumentFlags) doc->_versionedDoc.flags();
    if (doc->_versionedDoc.exists())
        flags = (C4DocumentFlags)(flags | kExists);
    return flags;
}


struct C4DocEnumerator {
    C4Database *_database;
    DocEnumerator _e;

    C4DocEnumerator(C4Database *database,
                    sequence start,
                    sequence end,
                    const DocEnumerator::Options& options)
    :_database(database),
     _e(*database->_db, start, end, options)
    { }

    C4DocEnumerator(C4Database *database,
                    C4Slice startDocID,
                    C4Slice endDocID,
                    const DocEnumerator::Options& options)
    :_database(database),
     _e(*database->_db, startDocID, endDocID, options)
    { }
};


void c4enum_free(C4DocEnumerator *e) {
    delete e;
}


C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                       C4SequenceNumber since,
                                       bool withBodies,
                                       C4Error *outError)
{
    auto options = DocEnumerator::Options::kDefault;
    options.inclusiveEnd = true;
    options.includeDeleted = false;
    if (!withBodies)
        options.contentOptions = KeyStore::kMetaOnly;
    return new C4DocEnumerator(database, since+1, UINT64_MAX, options);
}


C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                       C4Slice startDocID,
                                       C4Slice endDocID,
                                       bool descending,
                                       bool inclusiveEnd,
                                       unsigned skip,
                                       bool withBodies,
                                       C4Error *outError)
{
    auto options = DocEnumerator::Options::kDefault;
    options.skip = skip;
    options.descending = descending;
    options.inclusiveEnd = inclusiveEnd;
    if (!withBodies)
        options.contentOptions = KeyStore::kMetaOnly;
    return new C4DocEnumerator(database, startDocID, endDocID, options);
}


C4Document* c4enum_nextDocument(C4DocEnumerator *e) {
    if (!e->_e.next())
        return NULL;
    return new C4Document(e->_database, e->_e.doc());
}


#pragma mark - REVISIONS:


static C4Revision* c4rev_alloc(slice revID,
                               slice body)
{
    auto rev = new C4Revision;
    rev->revID = revID.copy();
    rev->body = body.copy();
    rev->flags = (C4RevisionFlags)0;
    rev->_rev = NULL;
    return rev;
}


static C4Revision* c4rev_alloc(const Revision* rev,
                               bool withBody =false,
                               C4Error *outError =NULL)
{
    if (!rev)
        return NULL;
    try {
        // Note: readBody() can throw
        auto c4rev = c4rev_alloc(rev->revID.expanded(),
                                 (withBody ? rev->readBody() : slice::null));
        c4rev->sequence = rev->sequence;
        c4rev->flags = (C4RevisionFlags)rev->flags;
        c4rev->_rev = (void*)rev;
        return c4rev;
    } catchError(outError);
    return NULL;
}


void c4rev_free(C4Revision* rev) {
    if (rev) {
        c4slice_free(rev->revID);
        c4slice_free(rev->body);
        delete rev;
    }
}


C4Revision* c4doc_getRevision(C4Document* doc, C4Slice revID, bool withBody, C4Error *outError) {
    const Revision *rev;
    if (revID.buf)
        rev = doc->_versionedDoc[revidBuffer(revID)];
    else
        rev = doc->_versionedDoc.currentRevision();
    return c4rev_alloc(rev, withBody, outError);
}


C4Revision* c4doc_getCurrentRevision(C4Document* doc, bool withBody, C4Error *outError) {
    return c4doc_getRevision(doc, kC4SliceNull, withBody, outError);
}


bool c4rev_loadBody(C4Revision *c4Rev, C4Error *outError) {
    if (c4Rev->body.buf)
        return true;
    try {
        auto rev = (const Revision*)c4Rev->_rev;
        c4Rev->body = rev->readBody().copy();
        if (c4Rev->body.buf)
            return true;
        recordHTTPError(410, outError); // 410 Gone to denote body that's been compacted away
    } catchError(outError);
    return false;
}


C4Revision* c4rev_getParent(C4Revision* c4rev) {
    auto rev = (const Revision*)c4rev->_rev;
    auto parentRevision = rev->parent();
    if (parentRevision)
        return c4rev_alloc(rev);
    return NULL;
}


C4Revision* c4rev_getNext(C4Revision* c4rev) {
    auto rev = (const Revision*)c4rev->_rev;
    return c4rev_alloc(rev->next());
}


C4Revision* c4rev_getNextLeaf(C4Revision* c4rev, bool includeDeleted, bool withBody,
                              C4Error *outError)
{
    auto rev = (const Revision*)c4rev->_rev;
    do {
        rev = rev->next();
    } while (rev && (!rev->isLeaf() || (!includeDeleted && rev->isDeleted())));
    return c4rev_alloc(rev, withBody, outError);
}


#pragma mark - INSERTING REVISIONS


bool c4doc_insertRevision(C4Document *doc,
                          C4Slice revID,
                          C4Slice body,
                          bool deleted,
                          bool hasAttachments,
                          C4Revision* parent,
                          bool allowConflict,
                          C4Error *outError)
{
    assert(doc->_db->inTransaction());
    try {
        auto parentRev = parent ? (const Revision*)parent->_rev : NULL;
        int httpStatus;
        auto newRev = doc->_versionedDoc.insert(revidBuffer(revID),
                                                body,
                                                deleted, hasAttachments, parentRev, allowConflict,
                                                httpStatus);
        if (newRev)
            return true;
        recordHTTPError(httpStatus, outError);
    } catchError(outError)
    return false;
}


int c4doc_insertRevisionWithHistory(C4Document *doc,
                                    C4Slice revID,
                                    C4Slice body,
                                    bool deleted,
                                    bool hasAttachments,
                                    C4Slice history[],
                                    unsigned historyCount,
                                    C4Error *outError)
{
    assert(doc->_db->inTransaction());
    int commonAncestor = -1;
    try {
        std::vector<revidBuffer> revIDBuffers;
        std::vector<revid> revIDs;
        revIDs.push_back(revidBuffer(revID));
        for (unsigned i = 0; i < historyCount; i++) {
            revIDBuffers.push_back(revidBuffer(history[i]));
            revIDs.push_back(revIDBuffers.back());
        }
        commonAncestor = doc->_versionedDoc.insertHistory(revIDs,
                                                          body,
                                                          deleted,
                                                          hasAttachments);
        if (commonAncestor < 0)
            recordHTTPError(400, outError); // must be invalid revision IDs
    } catchError(outError)
    return commonAncestor;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    slice result = doc->_versionedDoc.docType().copy();
    return (C4SliceResult){result.buf, result.size};
}

void c4doc_setType(C4Document *doc, C4Slice docType) {
    assert(doc->_db->inTransaction());
    doc->_versionedDoc.setDocType(docType);
}


bool c4doc_save(C4Document *doc,
                unsigned maxRevTreeDepth,
                C4Error *outError)
{
    assert(doc->_db->inTransaction());
    try {
        doc->_versionedDoc.prune(maxRevTreeDepth);
        doc->_versionedDoc.save(*doc->_db->transaction());
        return true;
    } catchError(outError)
    return false;
}
