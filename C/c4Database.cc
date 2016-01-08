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


c4Database::c4Database(std::string path, const config& cfg)
:Database(path, cfg),
 _transaction(NULL),
 _transactionLevel(0)
{ }

void c4Database::beginTransaction() {
#if C4DB_THREADSAFE
    _transactionMutex.lock(); // this is a recursive mutex
#endif
    if (++_transactionLevel == 1) {
        WITH_LOCK(this);
        _transaction = new Transaction(this);
    }
}

bool c4Database::inTransaction() {
#if C4DB_THREADSAFE
    std::lock_guard<std::recursive_mutex> lock(_transactionMutex);
#endif
    return _transactionLevel > 0;
}

bool c4Database::mustBeInTransaction(C4Error *outError) {
    if (inTransaction())
        return true;
    recordError(C4Domain, kC4ErrorNotInTransaction, outError);
    return false;
}

bool c4Database::mustNotBeInTransaction(C4Error *outError) {
    if (!inTransaction())
        return true;
    recordError(C4Domain, kC4ErrorTransactionNotClosed, outError);
    return false;
}

bool c4Database::endTransaction(bool commit) {
#if C4DB_THREADSAFE
    std::lock_guard<std::recursive_mutex> lock(_transactionMutex);
#endif
    if (_transactionLevel == 0)
        return false;
    if (--_transactionLevel == 0) {
        WITH_LOCK(this);
        auto t = _transaction;
        _transaction = NULL;
        if (!commit)
            t->abort();
        delete t; // this commits/aborts the transaction
    }
#if C4DB_THREADSAFE
    _transactionMutex.unlock(); // undoes lock in beginTransaction()
#endif
    return true;
}


namespace c4Internal {

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

}


C4Database* c4db_open(C4Slice path,
                      C4DatabaseFlags flags,
                      const C4EncryptionKey *encryptionKey,
                      C4Error *outError)
{
    auto pathStr = (std::string)path;
    auto config = c4DbConfig(flags, encryptionKey);
    try {
        try {
            return new c4Database(pathStr, config);
        } catch (cbforest::error error) {
            if (error.status == FDB_RESULT_INVALID_COMPACTION_MODE
                        && config.compaction_mode == FDB_COMPACTION_AUTO) {
                // Databases created by earlier builds of CBL (pre-1.2) didn't have auto-compact.
                // Opening them with auto-compact causes this error. Upgrade such a database by
                // switching its compaction mode:
                config.compaction_mode = FDB_COMPACTION_MANUAL;
                auto db = new c4Database(pathStr, config);
                db->setCompactionMode(FDB_COMPACTION_AUTO);
                return db;
            } else {
                throw error;
            }
        }
    }catchError(outError);
    return NULL;
}


bool c4db_close(C4Database* database, C4Error *outError) {
    if (database == NULL)
        return true;
    if (!database->mustNotBeInTransaction(outError))
        return false;
    WITH_LOCK(database);
    try {
        delete database;
        return true;
    } catchError(outError);
    return false;
}


bool c4db_delete(C4Database* database, C4Error *outError) {
    if (!database->mustNotBeInTransaction(outError))
        return false;
    WITH_LOCK(database);
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
    WITH_LOCK(database);
    try {
        database->compact();
        return true;
    } catchError(outError);
    return false;
}


bool c4db_rekey(C4Database* database, const C4EncryptionKey *newKey, C4Error *outError) {
    if (!database->mustNotBeInTransaction(outError))
        return false;
    WITH_LOCK(database);
    return rekey(database, newKey, outError);
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
        WITH_LOCK(database);
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
    WITH_LOCK(database);
    return database->lastSequence();
}


bool c4db_isInTransaction(C4Database* database) {
    WITH_LOCK(database);
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
        bool ok = database->endTransaction(commit);
        if (!ok)
            recordError(C4Domain, kC4ErrorNotInTransaction, outError);
        return ok;
    } catchError(outError);
    return false;
}


bool c4db_purgeDoc(C4Database *database, C4Slice docID, C4Error *outError) {
    WITH_LOCK(database);
    if (!database->mustBeInTransaction(outError))
        return false;
    try {
        database->transaction()->del(docID);
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
    WITH_LOCK(database);
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
    if (!c4db_beginTransaction(database, outError))
        return false;
    bool commit = false;
    try {
        WITH_LOCK(database);
        KeyStore localDocs(database, (std::string)storeName);
        KeyStoreWriter localWriter = (*database->transaction())(localDocs);
        if (body.buf || meta.buf)
            localWriter.set(key, meta, body);
        else
            localWriter.del(key);
        commit = true;
    } catchError(outError);
    c4db_endTransaction(database, commit, outError);
    return commit;
}
