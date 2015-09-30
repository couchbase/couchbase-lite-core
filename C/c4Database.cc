//
//  c4Database.cc
//  CBForest
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4Database.h"

#include "Database.hh"
#include "Document.hh"
#include "DocEnumerator.hh"
#include "LogInternal.hh"
#include "VersionedDocument.hh"
#include <assert.h>

using namespace forestdb;


// Size of ForestDB buffer cache allocated for a database
static const size_t kDBBufferCacheSize = (8*1024*1024);

// ForestDB Write-Ahead Log size (# of records)
static const size_t kDBWALThreshold = 1024;

// How often ForestDB should check whether databases need auto-compaction
static const uint64_t kAutoCompactInterval = (5*60);


void recordError(C4ErrorDomain domain, int code, C4Error* outError) {
    if (outError) {
        outError->domain = domain;
        outError->code = code;
    }
}

void recordHTTPError(int httpStatus, C4Error* outError) {
    recordError(HTTPDomain, httpStatus, outError);
}

void recordError(error e, C4Error* outError) {
    recordError(ForestDBDomain, e.status, outError);
}

void recordUnknownException(C4Error* outError) {
    Warn("Unexpected C++ exception thrown from CBForest");
    recordError(C4Domain, kC4ErrorInternalException, outError);
}


void c4slice_free(C4Slice slice) {
    slice.free();
}


static C4LogCallback clientLogCallback;

static void logCallback(logLevel level, const char *message) {
    auto cb = clientLogCallback;
    if (cb)
        cb((C4LogLevel)level, slice(message));
}


void c4log_register(C4LogLevel level, C4LogCallback callback) {
    if (callback) {
        LogLevel = (logLevel)level;
        LogCallback = logCallback;
    } else {
        LogLevel = kNone;
        LogCallback = NULL;
    }
    clientLogCallback = callback;
}


#pragma mark - DATABASES:


struct c4Database : public Database {

    c4Database(std::string path, const config& cfg)
    :Database(path, cfg),
     _transaction(NULL),
     _transactionLevel(0)
    { }

    void beginTransaction() {
        if (++_transactionLevel == 1)
            _transaction = new Transaction(this);
    }

    Transaction* transaction() {
        assert(_transaction);
        return _transaction;
    }

    bool inTransaction() { return _transactionLevel > 0; }

    bool mustBeInTransaction(C4Error *outError) {
        if (inTransaction())
            return true;
        recordError(C4Domain, kC4ErrorNotInTransaction, outError);
        return false;
    }

    bool mustNotBeInTransaction(C4Error *outError) {
        if (!inTransaction())
            return true;
        recordError(C4Domain, kC4ErrorTransactionNotClosed, outError);
        return false;
    }

    bool endTransaction(bool commit, C4Error *outError) {
        if (!mustBeInTransaction(outError))
            return false;
        if (--_transactionLevel == 0) {
            auto t = _transaction;
            _transaction = NULL;
            if (!commit)
                t->abort();
            delete t; // this commits/aborts the transaction
        }
        return true;
    }

private:
    Transaction* _transaction;
    int _transactionLevel;
};


forestdb::Database* asDatabase(C4Database *db) {
    return db;
}


Database::config c4DbConfig(C4DatabaseFlags flags, const C4EncryptionKey *key) {
    auto config = Database::defaultConfig();
    config.flags &= ~(FDB_OPEN_FLAG_RDONLY | FDB_OPEN_FLAG_CREATE);
    if (flags & kC4DB_ReadOnly)
        config.flags |= FDB_OPEN_FLAG_RDONLY;
    if (flags & kC4DB_Create)
        config.flags |= FDB_OPEN_FLAG_CREATE;
    config.buffercache_size = kDBBufferCacheSize;
    config.wal_threshold = kDBWALThreshold;
    config.wal_flush_before_commit = true;
    config.seqtree_opt = true;
    config.compress_document_body = true;
    config.compaction_mode = (flags & kC4DB_AutoCompact) ? FDB_COMPACTION_AUTO : FDB_COMPACTION_MANUAL;
    config.compactor_sleep_duration = kAutoCompactInterval; // global to all databases
    if (key) {
        config.encryption_key.algorithm = key->algorithm;
        memcpy(config.encryption_key.bytes, key->bytes, sizeof(config.encryption_key.bytes));
    }
    return config;
}


C4Database* c4db_open(C4Slice path,
                      C4DatabaseFlags flags,
                      const C4EncryptionKey *encryptionKey,
                      C4Error *outError)
{
    try {
        return new c4Database((std::string)path, c4DbConfig(flags, encryptionKey));
    } catchError(outError);
    return NULL;
}


bool c4db_close(C4Database* database, C4Error *outError) {
    if (database == NULL)
        return true;
    if (!database->mustNotBeInTransaction(outError))
        return false;
    try {
        delete database;
        return true;
    } catchError(outError);
    return false;
}


bool c4db_delete(C4Database* database, C4Error *outError) {
    if (!database->mustNotBeInTransaction(outError))
        return false;
    try {
        database->deleteDatabase();
        delete database;
        return true;
    } catchError(outError);
    return false;
}


uint64_t c4db_getDocumentCount(C4Database* database) {
    try {
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = Database::kMetaOnly;

        uint64_t count = 0;
        for (DocEnumerator e(*database, forestdb::slice::null, forestdb::slice::null, opts);
                e.next(); ) {
            VersionedDocument vdoc(*database, *e);
            if (!vdoc.isDeleted())
                ++count;
        }
        return count;
    } catchError(NULL);
    return 0;
}


C4SequenceNumber c4db_getLastSequence(C4Database* database) {
    return database->lastSequence();
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
        return database->endTransaction(commit, outError);
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
        KeyStore localDocs(database, (std::string)storeName);
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
        KeyStore localDocs(database, (std::string)storeName);
        KeyStoreWriter localWriter = (*database->transaction())(localDocs);
        if (body.buf || meta.buf)
            localWriter.set(key, meta, body);
        else
            localWriter.del(key);
        abort = false;
        return database->endTransaction(true, outError);
    } catchError(outError);
    if (abort)
        database->endTransaction(false, NULL);
    return false;
}


#pragma mark - DOCUMENTS:


struct C4DocumentInternal : public C4Document {
    C4Database* _db;
    VersionedDocument _versionedDoc;
    const Revision *_selectedRev;
    alloc_slice _revIDBuf;
    alloc_slice _selectedRevIDBuf;
    alloc_slice _loadedBody;

    C4DocumentInternal(C4Database* database, C4Slice docID)
    :_db(database),
     _versionedDoc(*_db, docID),
     _selectedRev(NULL)
    {
        init();
    }

    C4DocumentInternal(C4Database *database, const Document &doc)
    :_db(database),
     _versionedDoc(*_db, doc),
     _selectedRev(NULL)
    {
        init();
    }

    void init() {
        docID = _versionedDoc.docID();
        flags = (C4DocumentFlags)_versionedDoc.flags();
        if (_versionedDoc.exists())
            flags = (C4DocumentFlags)(flags | kExists);
        initRevID();
        if (_versionedDoc.revsAvailable()) {
            selectRevision(_versionedDoc.currentRevision());
        } else {
            // Doc body (rev tree) isn't available, but we know most things about the current rev:
            selectedRev.revID = revID;
            selectedRev.sequence = sequence;
            int revFlags = 0;
            if (flags & kExists) {
                revFlags |= kRevLeaf;
                if (flags & kDeleted)
                    revFlags |= kRevDeleted;
                if (flags & kHasAttachments)
                    revFlags |= kRevHasAttachments;
            }
            selectedRev.flags = (C4RevisionFlags)revFlags;
        }
    }

    void initRevID() {
        _revIDBuf = _versionedDoc.revID().expanded();
        revID = _revIDBuf;
        sequence = _versionedDoc.sequence();
    }

    bool selectRevision(const Revision *rev, C4Error *outError =NULL) {
        _selectedRev = rev;
        _loadedBody = slice::null;
        if (rev) {
            _selectedRevIDBuf = rev->revID.expanded();
            selectedRev.revID = _selectedRevIDBuf;
            selectedRev.flags = (C4RevisionFlags)rev->flags;
            selectedRev.sequence = rev->sequence;
            selectedRev.body = rev->inlineBody();
            return true;
        } else {
            _selectedRevIDBuf = slice::null;
            selectedRev.revID = slice::null;
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = slice::null;
            recordHTTPError(404, outError);
            return false;
        }
    }

    bool loadRevisions(C4Error *outError) {
        if (_versionedDoc.revsAvailable())
            return true;
        try {
            _versionedDoc.read();
            _selectedRev = _versionedDoc.currentRevision();
            return true;
        } catchError(outError)
        return false;
    }

    bool loadSelectedRevBody(C4Error *outError) {
        if (!loadRevisions(outError))
            return false;
        if (!_selectedRev)
            return true;
        if (selectedRev.body.buf)
            return true;  // already loaded
        try {
            _loadedBody = _selectedRev->readBody();
            selectedRev.body = _loadedBody;
            if (_loadedBody.buf)
                return true;
            recordHTTPError(410, outError); // 410 Gone to denote body that's been compacted away
        } catchError(outError);
        return false;
    }

    void updateMeta() {
        _versionedDoc.updateMeta();
        flags = (C4DocumentFlags)(_versionedDoc.flags() | kExists);
        initRevID();
    }
};

static inline C4DocumentInternal *internal(C4Document *doc) {
    return (C4DocumentInternal*)doc;
}


void c4doc_free(C4Document *doc) {
    delete (C4DocumentInternal*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        auto doc = new C4DocumentInternal(database, docID);
        if (mustExist && !doc->_versionedDoc.exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError)
{
    auto idoc = internal(doc);
    if (revID.buf) {
        if (!idoc->loadRevisions(outError))
            return false;
        const Revision *rev = idoc->_versionedDoc[revidBuffer(revID)];
        return idoc->selectRevision(rev, outError) && (!withBody || idoc->loadSelectedRevBody(outError));
    } else {
        idoc->selectRevision(NULL);
        return true;
    }
}


bool c4doc_selectCurrentRevision(C4Document* doc)
{
    auto idoc = internal(doc);
    if (!idoc->loadRevisions(NULL))
        return false;
    const Revision *rev = idoc->_versionedDoc.currentRevision();
    return idoc->selectRevision(rev);
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) {
    return internal(doc)->loadSelectedRevBody(outError);
}


bool c4doc_selectParentRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->parent());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->next());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError)
{
    auto idoc = internal(doc);
    auto rev = idoc->_selectedRev;
    if (rev) {
        do {
            rev = rev->next();
        } while (rev && (!rev->isLeaf() || (!includeDeleted && rev->isDeleted())));
    }
    return idoc->selectRevision(rev, outError) && (!withBody || idoc->loadSelectedRevBody(outError));
}


#pragma mark - INSERTING REVISIONS


bool c4doc_insertRevision(C4Document *doc,
                          C4Slice revID,
                          C4Slice body,
                          bool deleted,
                          bool hasAttachments,
                          bool allowConflict,
                          C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->_db->mustBeInTransaction(outError))
        return false;
    if (!idoc->loadRevisions(NULL))
        return false;
    try {
        revidBuffer encodedRevID(revID);
        int httpStatus;
        auto newRev = idoc->_versionedDoc.insert(encodedRevID,
                                                 body,
                                                 deleted,
                                                 hasAttachments,
                                                 idoc->_selectedRev,
                                                 allowConflict,
                                                 httpStatus);
        if (newRev) {
            idoc->updateMeta();
            newRev = idoc->_versionedDoc.get(encodedRevID);
            return idoc->selectRevision(newRev);
        }
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
    auto idoc = internal(doc);
    if (!idoc->_db->mustBeInTransaction(outError))
        return -1;
    if (!idoc->loadRevisions(NULL))
        return false;
    int commonAncestor = -1;
    try {
        std::vector<revidBuffer> revIDBuffers;
        std::vector<revid> revIDs;
        revIDs.push_back(revidBuffer(revID));
        for (unsigned i = 0; i < historyCount; i++) {
            revIDBuffers.push_back(revidBuffer(history[i]));
            revIDs.push_back(revIDBuffers.back());
        }
        commonAncestor = idoc->_versionedDoc.insertHistory(revIDs,
                                                           body,
                                                           deleted,
                                                           hasAttachments);
        if (commonAncestor >= 0) {
            idoc->updateMeta();
            idoc->selectRevision(idoc->_versionedDoc[revidBuffer(revID)]);
        } else {
            recordHTTPError(400, outError); // must be invalid revision IDs
        }
    } catchError(outError)
    return commonAncestor;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    slice result = internal(doc)->_versionedDoc.docType().copy();
    return {result.buf, result.size};
}

bool c4doc_setType(C4Document *doc, C4Slice docType, C4Error *outError) {
    auto idoc = internal(doc);
    if (!idoc->_db->mustBeInTransaction(outError))
        return false;
    idoc->_versionedDoc.setDocType(docType);
    return true;
}


bool c4doc_save(C4Document *doc,
                unsigned maxRevTreeDepth,
                C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->_db->mustBeInTransaction(outError))
        return false;
    try {
        idoc->_versionedDoc.prune(maxRevTreeDepth);
        idoc->_versionedDoc.save(*idoc->_db->transaction());
        return true;
    } catchError(outError)
    return false;
}


#pragma mark - DOC ENUMERATION:

const C4AllDocsOptions kC4DefaultAllDocsOptions = {
	false,
    true,
    true,
	0,
	false,
    true
};

const C4ChangesOptions kC4DefaultChangesOptions = {
    false,
	true
};


struct C4DocEnumerator {
    C4Database *_database;
    DocEnumerator _e;

    C4DocEnumerator(C4Database *database,
                    sequence start,
                    sequence end,
                    const DocEnumerator::Options& options)
    :_database(database),
     _e(*database, start, end, options)
    { }

    C4DocEnumerator(C4Database *database,
                    C4Slice startDocID,
                    C4Slice endDocID,
                    const DocEnumerator::Options& options)
    :_database(database),
     _e(*database, startDocID, endDocID, options)
    { }
};


void c4enum_free(C4DocEnumerator *e) {
    delete e;
}


C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                       C4SequenceNumber since,
                                       const C4ChangesOptions *c4options,
                                       C4Error *outError)
{
    try {
        if (!c4options)
            c4options = &kC4DefaultChangesOptions;
        auto options = DocEnumerator::Options::kDefault;
        options.inclusiveEnd = true;
        options.includeDeleted = c4options->includeDeleted;
        if (!c4options->includeBodies)
            options.contentOptions = KeyStore::kMetaOnly;
        return new C4DocEnumerator(database, since+1, UINT64_MAX, options);
    } catchError(outError);
    return NULL;
}


C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                       C4Slice startDocID,
                                       C4Slice endDocID,
                                       const C4AllDocsOptions *c4options,
                                       C4Error *outError)
{
    try {
        if (!c4options)
            c4options = &kC4DefaultAllDocsOptions;
        auto options = DocEnumerator::Options::kDefault;
        options.skip = c4options->skip;
        options.descending = c4options->descending;
        options.inclusiveStart = c4options->inclusiveStart;
        options.inclusiveEnd = c4options->inclusiveEnd;
        options.includeDeleted = c4options->includeDeleted;
        if (!c4options->includeBodies)
            options.contentOptions = KeyStore::kMetaOnly;
        return new C4DocEnumerator(database, startDocID, endDocID, options);
    } catchError(outError);
    return NULL;
}


C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError) {
    try {
        if (e->_e.next())
            return new C4DocumentInternal(e->_database, e->_e.doc());
        recordError(FDB_RESULT_SUCCESS, outError);      // end of iteration is not an error
    } catchError(outError)
    return NULL;
}
