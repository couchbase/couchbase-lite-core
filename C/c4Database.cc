//
//  c4Database.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/8/15.
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
#include "c4DatabaseInternal.hh"
#include "c4Database.h"
#include "c4Private.h"

#include "ForestDataFile.hh"
#include "SQLiteDataFile.hh"
#include "KeyStore.hh"
#include "Document.hh"
#include "DocEnumerator.hh"
#include "LogInternal.hh"

#include "Collatable.hh"
#include "FilePath.hh"


const char* const kC4ForestDBStorageEngine = "ForestDB";
const char* const kC4SQLiteStorageEngine   = "SQLite";


#pragma mark - C4DATABASE CLASS:


// `path` is path to bundle; return value is path to db file. Updates config.storageEngine. */
FilePath c4Database::findOrCreateBundle(const string &path, C4DatabaseConfig &config) {
    FilePath bundle {path, ""};
    bool createdDir = ((config.flags & kC4DB_Create) && bundle.mkdir());
    if (!createdDir)
        bundle.mustExistAsDir();

    DataFile::Factory *factory = DataFile::factoryNamed(config.storageEngine);
    if (!factory)
        error::_throw(error::InvalidParameter);

    // Look for the file corresponding to the requested storage engine (defaulting to SQLite):

    FilePath dbFile = bundle["db"].withExtension(factory->filenameExtension());
    if (createdDir || factory->fileExists(dbFile)) {
        if (config.storageEngine == nullptr)
            config.storageEngine = factory->cname();
        return dbFile;
    }

    if (config.storageEngine != nullptr) {
        // DB exists but not in the format they specified, so fail:
        error::_throw(error::WrongFormat);
    }

    // Not found, but they didn't specify a format, so try the other formats:
    for (auto otherFactory : DataFile::factories()) {
        if (otherFactory != factory) {
            dbFile = bundle["db"].withExtension(otherFactory->filenameExtension());
            if (factory->fileExists(dbFile)) {
                config.storageEngine = factory->cname();
                return dbFile;
            }
        }
    }

    // Weird; the bundle exists but doesn't contain any known type of database, so fail:
    error::_throw(error::WrongFormat);
}


c4Database* c4Database::newDatabase(string pathStr, C4DatabaseConfig config) {
    FilePath path = (config.flags & kC4DB_Bundled)
                        ? findOrCreateBundle(pathStr, config)
                        : FilePath(pathStr);
    switch (config.versioning) {
        case kC4VersionVectors: return (new c4DatabaseV2((string)path, config))->retain();
        case kC4RevisionTrees:  return (new c4DatabaseV1((string)path, config))->retain();
        default:                error::_throw(error::InvalidParameter);
    }
}


/*static*/ DataFile* c4Database::newDataFile(string path,
                                  const C4DatabaseConfig &config,
                                  bool isMainDB)
{
    DataFile::Options options { };
    if (isMainDB) {
        options.keyStores.sequences = options.keyStores.softDeletes = true;
        options.keyStores.getByOffset = (config.versioning == kC4RevisionTrees);
    }
    options.create = (config.flags & kC4DB_Create) != 0;
    options.writeable = (config.flags & kC4DB_ReadOnly) == 0;

    options.encryptionAlgorithm = (DataFile::EncryptionAlgorithm)config.encryptionKey.algorithm;
    if (options.encryptionAlgorithm != DataFile::kNoEncryption) {
        options.encryptionKey = alloc_slice(config.encryptionKey.bytes,
                                            sizeof(config.encryptionKey.bytes));
    }

    DataFile::Factory *storage = DataFile::factoryNamed((string)(config.storageEngine ?: ""));
    if (!storage)
        error::_throw(error::Unimplemented);
    return storage->openFile(path, &options);
}


c4Database::c4Database(string path,
                       const C4DatabaseConfig &inConfig)
:config(inConfig),
 _db(newDataFile(path, config, true))
{
    // Validate that the versioning matches what's used in the database:
    auto &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
    Document doc = info.get(slice("versioning"));
    if (doc.exists()) {
        if (doc.bodyAsUInt() != (uint64_t)config.versioning)
            error::_throw(error::WrongFormat);
    } else if (config.flags & kC4DB_Create) {
        doc.setBodyAsUInt((uint64_t)config.versioning);
        Transaction t(*_db);
        info.write(doc, t);
    } else if (config.versioning != kC4RevisionTrees) {
        error::_throw(error::WrongFormat);
    }
}

bool c4Database::mustUseVersioning(C4DocumentVersioning requiredVersioning, C4Error *outError) {
    if (config.versioning == requiredVersioning)
        return true;
    recordError(LiteCoreDomain, kC4ErrorUnsupported, outError);
    return false;
}


void c4Database::beginTransaction() {
#if C4DB_THREADSAFE
    _transactionMutex.lock(); // this is a recursive mutex
#endif
    if (++_transactionLevel == 1) {
        WITH_LOCK(this);
        _transaction = new Transaction(_db.get());
    }
}

bool c4Database::inTransaction() {
#if C4DB_THREADSAFE
    lock_guard<recursive_mutex> lock(_transactionMutex);
#endif
    return _transactionLevel > 0;
}

bool c4Database::mustBeInTransaction(C4Error *outError) {
    if (inTransaction())
        return true;
    recordError(LiteCoreDomain, kC4ErrorNotInTransaction, outError);
    return false;
}

bool c4Database::mustNotBeInTransaction(C4Error *outError) {
    if (!inTransaction())
        return true;
    recordError(LiteCoreDomain, kC4ErrorTransactionNotClosed, outError);
    return false;
}

bool c4Database::endTransaction(bool commit) {
#if C4DB_THREADSAFE
    lock_guard<recursive_mutex> lock(_transactionMutex);
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


/*static*/ bool c4Database::rekey(DataFile* database, const C4EncryptionKey *newKey,
                                  C4Error *outError)
{
    try {
        if (newKey) {
            database->rekey((DataFile::EncryptionAlgorithm)newKey->algorithm,
                            slice(newKey->bytes, 32));
        } else {
            database->rekey(DataFile::kNoEncryption, slice::null);
        }
        return true;
    } catchError(outError);
    return false;
}


#pragma mark - DATABASE API:


C4Database* c4db_open(C4Slice path,
                      const C4DatabaseConfig *configP,
                      C4Error *outError)
{
    if (!checkParam(configP != nullptr, outError))
        return nullptr;
    try {
        return c4Database::newDatabase((string)path, *configP);
    } catchError(outError);
    return nullptr;
}


bool c4db_close(C4Database* database, C4Error *outError) {
    if (database == NULL)
        return true;
    if (!database->mustNotBeInTransaction(outError))
        return false;
    WITH_LOCK(database);
    try {
        database->db()->close();
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
    if (database->refCount() > 1) {
        recordError(LiteCoreDomain, kC4ErrorBusy, outError);
        return false;
    }
    try {
        if (database->config.flags & kC4DB_Bundled) {
            FilePath bundle = database->db()->filePath().dir();
            database->db()->close();
            bundle.delRecursive();
        } else {
            database->db()->deleteDataFile();
        }
        return true;
    } catchError(outError);
    return false;
}


bool c4db_deleteAtPath(C4Slice dbPath, const C4DatabaseConfig *config, C4Error *outError) {
    try {
        if (config == nullptr) {
            FilePath((string)dbPath).delWithAllExtensions();
        } else if (config->flags & kC4DB_Bundled) {
            FilePath bundle{(string)dbPath, ""};
            bundle.delRecursive();
        } else {
            FilePath path((string)dbPath);
            DataFile::Factory *factory = nullptr;
            if (config && config->storageEngine) {
                factory = DataFile::factoryNamed(config->storageEngine);
                if (!factory)
                    Warn("c4db_deleteAtPath: unknown storage engine '%s'", config->storageEngine);
            } else {
                factory = DataFile::factoryForFile(path);
                if (!factory)
                    factory = DataFile::factories()[0];
            }
            if (!factory)
                error::_throw(error::WrongFormat);
            factory->deleteFile(path);
        }
        return true;
    } catchError(outError);
    return false;
}


bool c4db_compact(C4Database* database, C4Error *outError) {
    if (!database->mustNotBeInTransaction(outError))
        return false;
    WITH_LOCK(database);
    try {
        database->db()->compact();
        return true;
    } catchError(outError);
    return false;
}


bool c4db_isCompacting(C4Database *database) {
    return database ? database->db()->isCompacting() : DataFile::isAnyCompacting();
}

void c4db_setOnCompactCallback(C4Database *database, C4OnCompactCallback cb, void *context) {
    WITH_LOCK(database);
    database->db()->setOnCompact([cb,context](bool compacting) {
        cb(context, compacting);
    });
}


bool c4db_rekey(C4Database* database, const C4EncryptionKey *newKey, C4Error *outError) {
    if (!database->mustNotBeInTransaction(outError))
        return false;
    WITH_LOCK(database);
    return c4Database::rekey(database->db(), newKey, outError);
}


C4SliceResult c4db_getPath(C4Database *database) {
    FilePath path = database->db()->filePath();
    if (database->config.flags & kC4DB_Bundled)
        path = path.dir();
    slice s = slice(path.path()).copy();  // C4SliceResult must be malloced & adopted by caller
    return {s.buf, s.size};
}


const C4DatabaseConfig* c4db_getConfig(C4Database *database) {
    return &database->config;
}


uint64_t c4db_getDocumentCount(C4Database* database) {
    try {
        WITH_LOCK(database);
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = kMetaOnly;

        uint64_t count = 0;
        for (DocEnumerator e(database->defaultKeyStore(), slice::null, slice::null, opts);
                e.next(); ) {
            C4DocumentFlags flags;
            if (database->readDocMeta(e.doc(), &flags) && !(flags & kDeleted))
                ++count;
        }
        return count;
    } catchError(NULL);
    return 0;
}


C4SequenceNumber c4db_getLastSequence(C4Database* database) {
    WITH_LOCK(database);
    try {
        return database->defaultKeyStore().lastSequence();
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
            recordError(LiteCoreDomain, kC4ErrorNotInTransaction, outError);
        return ok;
    } catchError(outError);
    return false;
}


bool c4db_purgeDoc(C4Database *database, C4Slice docID, C4Error *outError) {
    WITH_LOCK(database);
    if (!database->mustBeInTransaction(outError))
        return false;
    try {
        if (database->defaultKeyStore().del(docID, database->transaction()))
            return true;
        else
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
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
    try {
        ForestDataFile::shutdown();
        SQLiteDataFile::shutdown();
        return true;
    } catchError(NULL) {
        return false;
    }
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
        KeyStore& localDocs = database->getKeyStore((string)storeName);
        Document doc = localDocs.get(key);
        if (!doc.exists()) {
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
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
        KeyStore &localDocs = database->getKeyStore((string)storeName);
        auto &t = database->transaction();
        if (body.buf || meta.buf)
            localDocs.set(key, meta, body, t);
        else
            localDocs.del(key, t);
        commit = true;
    } catchError(outError);
    c4db_endTransaction(database, commit, outError);
    return commit;
}
