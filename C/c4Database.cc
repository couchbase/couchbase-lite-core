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

using namespace cbforest;


// Size of ForestDB buffer cache allocated for a database
static const size_t kDBBufferCacheSize = (8*1024*1024);

// ForestDB Write-Ahead Log size (# of records)
static const size_t kDBWALThreshold = 1024;

// How often ForestDB should check whether databases need auto-compaction
static const uint64_t kAutoCompactInterval = (5*60);


namespace c4Internal {
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
}


bool c4SliceEqual(C4Slice a, C4Slice b) {
    return a == b;
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
        CBFAssert(_transaction);
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


namespace c4Internal {

    cbforest::Database* asDatabase(C4Database *db) {
        return db;
    }

    bool mustBeInTransaction(C4Database *db, C4Error *outError) {
        return db->mustBeInTransaction(outError);
    }

    Database::config c4DbConfig(C4DatabaseFlags flags, const C4EncryptionKey *key) {
        auto config = Database::defaultConfig();
        // global to all databases:
        config.buffercache_size = kDBBufferCacheSize;
        config.compress_document_body = true;
        config.compactor_sleep_duration = kAutoCompactInterval;
        config.num_compactor_threads = 1;
        config.num_bgflusher_threads = 1;

        // per-database settings:
        config.flags &= ~(FDB_OPEN_FLAG_RDONLY | FDB_OPEN_FLAG_CREATE);
        if (flags & kC4DB_ReadOnly)
            config.flags |= FDB_OPEN_FLAG_RDONLY;
        if (flags & kC4DB_Create)
            config.flags |= FDB_OPEN_FLAG_CREATE;
        config.wal_threshold = kDBWALThreshold;
        config.wal_flush_before_commit = true;
        config.seqtree_opt = true;
        config.compaction_mode = (flags & kC4DB_AutoCompact) ? FDB_COMPACTION_AUTO : FDB_COMPACTION_MANUAL;
        if (key) {
            config.encryption_key.algorithm = key->algorithm;
            memcpy(config.encryption_key.bytes, key->bytes, sizeof(config.encryption_key.bytes));
        }
        return config;
    }

    Document dbGetDoc(C4Database *db, sequence seq) {
        return db->get(seq);
    }

    Transaction* dbGetTransaction(C4Database *db) {
        return db->transaction();
    }
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


bool c4db_compact(C4Database* database, C4Error *outError) {
    if (!database->mustNotBeInTransaction(outError))
        return false;
    try {
        database->compact();
        return true;
    } catchError(outError);
    return false;
}


bool c4db_rekey(C4Database* database, const C4EncryptionKey *newKey, C4Error *outError) {
    return database->mustNotBeInTransaction(outError)
        && rekey(database, newKey, outError);
}


bool c4Internal::rekey(Database* database, const C4EncryptionKey *newKey,
                                 C4Error *outError) {
    try {
        fdb_encryption_key key = {FDB_ENCRYPTION_NONE, {}};
        if (newKey) {
            key.algorithm = newKey->algorithm;
            memcpy(key.bytes, newKey->bytes, sizeof(key.bytes));
        }
        database->rekey(key);
        return true;
    } catchError(outError);
    return false;
}


uint64_t c4db_getDocumentCount(C4Database* database) {
    try {
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = Database::kMetaOnly;

        uint64_t count = 0;
        for (DocEnumerator e(*database, cbforest::slice::null, cbforest::slice::null, opts);
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


bool c4db_purgeDoc(C4Database *db, C4Slice docID, C4Error *outError) {
    if (!db->mustBeInTransaction(outError))
        return false;
    try {
        db->transaction()->del(docID);
        return true;
    } catchError(outError)
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
