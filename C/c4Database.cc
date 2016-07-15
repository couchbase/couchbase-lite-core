//
//  c4Database.cc
//  CBForest
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "c4Impl.hh"
#include "c4Database.h"
#include "c4Private.h"

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
    std::atomic_int InstanceCounted::gObjectCount;

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) {
        if (outError) {
            if (domain == ForestDBDomain && code <= -1000)   // custom CBForest errors (Error.hh)
                domain = C4Domain;
            outError->domain = domain;
            outError->code = code;
        }
    }

    void recordHTTPError(int httpStatus, C4Error* outError) {
        recordError(HTTPDomain, httpStatus, outError);
    }

    void recordError(const error &e, C4Error* outError) {
        recordError(ForestDBDomain, e.status, outError);
    }

    void recordException(const std::exception &e, C4Error* outError) {
        Warn("Unexpected C++ \"%s\" exception thrown from CBForest", e.what());
        recordError(C4Domain, kC4ErrorInternalException, outError);
    }

    void recordUnknownException(C4Error* outError) {
        Warn("Unexpected C++ exception thrown from CBForest");
        recordError(C4Domain, kC4ErrorInternalException, outError);
    }
}


C4SliceResult c4error_getMessage(C4Error error) {
    if (error.code == 0)
        return {NULL, 0};
    
    const char *msg = NULL;
    switch (error.domain) {
        case ForestDBDomain:
            msg = fdb_error_msg((fdb_status)error.code);
            if (strcmp(msg, "unknown error") == 0)
                msg = NULL;
            break;
        case POSIXDomain:
            msg = strerror(error.code);
            break;
        case HTTPDomain:
            switch (error.code) {
                case kC4HTTPBadRequest: msg = "invalid parameter"; break;
                case kC4HTTPNotFound:   msg = "not found"; break;
                case kC4HTTPConflict:   msg = "conflict"; break;
                case kC4HTTPGone:       msg = "gone"; break;
                default: break;
            }
        case C4Domain:
            switch (error.code) {
                case kC4ErrorInternalException:     msg = "internal exception"; break;
                case kC4ErrorNotInTransaction:      msg = "no transaction is open"; break;
                case kC4ErrorTransactionNotClosed:  msg = "a transaction is still open"; break;
                case kC4ErrorIndexBusy:             msg = "index busy; can't close view"; break;
                case kC4ErrorBadRevisionID:         msg = "invalid revision ID"; break;
                case kC4ErrorCorruptRevisionData:   msg = "corrupt revision data"; break;
                case kC4ErrorCorruptIndexData:      msg = "corrupt view-index data"; break;
                case kC4ErrorAssertionFailed:       msg = "internal assertion failure"; break;
                case kC4ErrorTokenizerError:        msg = "full-text tokenizer error"; break;
                default: break;
            }
    }

    char buf[100];
    if (!msg) {
        const char* const kDomainNames[4] = {"HTTP", "POSIX", "ForestDB", "CBForest"};
        if (error.domain <= C4Domain)
            sprintf(buf, "unknown %s error %d", kDomainNames[error.domain], error.code);
        else
            sprintf(buf, "bogus C4Error (%d, %d)", error.domain, error.code);
        msg = buf;
    }

    slice result = alloc_slice(msg, strlen(msg)).dontFree();
    return {result.buf, result.size};
}


int c4_getObjectCount() {
    return InstanceCounted::gObjectCount;
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
:Database(path, cfg)
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
        config.seqtree_opt = FDB_SEQTREE_USE;
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
        database->close();
        return true;
    } catchError(outError);
    return false;
}


bool c4db_free(C4Database* database) {
    if (database == NULL)
        return true;
    if (!database->mustNotBeInTransaction(NULL))
        return false;
    WITH_LOCK(database);
    try {
        database->release();
        return true;
    } catchError(NULL);
    return false;
}


bool c4db_delete(C4Database* database, C4Error *outError) {
    if (!database->mustNotBeInTransaction(outError))
        return false;
    WITH_LOCK(database);
    try {
        if (database->refCount() > 1) {
            recordError(ForestDBDomain, FDB_RESULT_FILE_IS_BUSY, outError);
        }
        database->deleteDatabase();
        return true;
    } catchError(outError);
    return false;
}


bool c4db_deleteAtPath(C4Slice dbPath, C4DatabaseFlags flags, C4Error *outError) {
    try {
        auto config = c4DbConfig(flags, NULL);
        Database::deleteDatabase((std::string)dbPath, config);
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


bool c4db_isCompacting(C4Database *database) {
    return database ? database->isCompacting() : Database::isAnyCompacting();
}

void c4db_setOnCompactCallback(C4Database *database, C4OnCompactCallback cb, void *context) {
    WITH_LOCK(database);
    database->setOnCompact(cb, context);
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


C4SliceResult c4db_getPath(C4Database *database) {
    slice path(database->getInfo().filename);
    path = path.copy();  // C4SliceResult must be malloced & adopted by caller
    return {path.buf, path.size};
}


uint64_t c4db_getDocumentCount(C4Database* database) {
    try {
        WITH_LOCK(database);
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = Database::kMetaOnly;

        uint64_t count = 0;
        for (DocEnumerator e(*database, cbforest::slice::null, cbforest::slice::null, opts);
                e.next(); ) {
            VersionedDocument vdoc(*database, e.moveDoc());
            if (!vdoc.isDeleted())
                ++count;
        }
        return count;
    } catchError(NULL);
    return 0;
}


C4SequenceNumber c4db_getLastSequence(C4Database* database) {
    WITH_LOCK(database);
    try {
        return database->lastSequence();
    } catchError(NULL);
    return 0;
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
        if (database->transaction()->del(docID))
            return true;
        else
            recordError(ForestDBDomain, FDB_RESULT_KEY_NOT_FOUND, outError);
    } catchError(outError)
    return false;
}

uint64_t c4db_nextDocExpiration(C4Database *database)
{
    try {
        WITH_LOCK(database);
        KeyStore& expiryKvs = database->getKeyStore("expiry");
        DocEnumerator e(expiryKvs);
        if(e.next() && e.doc().body() == slice::null) {
            // Look for an entry with a null body (otherwise, its key is simply a doc ID)
            CollatableReader r(e.doc().key());
            r.beginArray();
            return (uint64_t)r.readInt();
        }
    } catchError(NULL)
    return 0ul;
}

bool c4_shutdown(C4Error *outError) {
    fdb_status err = fdb_shutdown();
    if (err) {
        recordError(err, outError);
        return false;
    }
    return true;
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
        KeyStore& localDocs = database->getKeyStore((std::string)storeName);
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
        KeyStore &localDocs = database->getKeyStore((std::string)storeName);
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
