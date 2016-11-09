//
//  Database.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/19/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Database.hh"
#include "Document.hh"
#include "c4Internal.hh"
#include "c4Document.h"
#include "DataFile.hh"
#include "Collatable.hh"
#include "CASRevisionStore.hh"
#include "SequenceTracker.hh"
#include "Fleece.hh"


namespace c4Internal {
    using namespace litecore;


#pragma mark - LIFECYCLE:


    // `path` is path to bundle; return value is path to db file. Updates config.storageEngine. */
    FilePath Database::findOrCreateBundle(const string &path, C4DatabaseConfig &config) {
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


    Database* Database::newDatabase(const string &pathStr, C4DatabaseConfig config) {
        FilePath path = (config.flags & kC4DB_Bundled)
                            ? findOrCreateBundle(pathStr, config)
                            : FilePath(pathStr);
        Retained<Database> db {new Database((string)path, config)};
        DocumentFactory* factory;
        switch (config.versioning) {
            case kC4VersionVectors: factory = new VectorDocumentFactory(db); break;
            case kC4RevisionTrees:  factory = new TreeDocumentFactory(db); break;
            default:                error::_throw(error::InvalidParameter);
        }
        db->_documentFactory.reset(factory);
        return db->retain();
    }


    /*static*/ DataFile* Database::newDataFile(const string &path,
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

        options.encryptionAlgorithm = (EncryptionAlgorithm)config.encryptionKey.algorithm;
        if (options.encryptionAlgorithm != kNoEncryption) {
            options.encryptionKey = alloc_slice(config.encryptionKey.bytes,
                                                sizeof(config.encryptionKey.bytes));
        }

        const char *storageEngine = config.storageEngine;
        if (!storageEngine) {
            storageEngine = "";
        }

        DataFile::Factory *storage = DataFile::factoryNamed((string)(storageEngine));
        if (!storage)
            error::_throw(error::Unimplemented);
        return storage->openFile(path, &options);
    }


    Database::Database(const string &path,
                       const C4DatabaseConfig &inConfig)
    :config(inConfig),
     _db(newDataFile(path, config, true)),
     _encoder(new fleece::Encoder()),
     _sequenceTracker(new SequenceTracker())
    {
        if (config.flags & kC4DB_SharedKeys) {
            _db->useDocumentKeys();
            _encoder->setSharedKeys(documentKeys());
        }

        // Validate that the versioning matches what's used in the database:
        auto &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
        Record doc = info.get(slice("versioning"));
        if (doc.exists()) {
            if (doc.bodyAsUInt() != (uint64_t)config.versioning)
                error::_throw(error::WrongFormat);
        } else if (config.flags & kC4DB_Create) {
            doc.setBodyAsUInt((uint64_t)config.versioning);
            Transaction t(*_db);
            info.write(doc, t);
            t.commit();
        } else if (config.versioning != kC4RevisionTrees) {
            error::_throw(error::WrongFormat);
        }
    }


    Database::~Database() {
        Assert(_transactionLevel == 0);
    }


#pragma mark - HOUSEKEEPING:


    void Database::close() {
        mustNotBeInTransaction();
        WITH_LOCK(this);
        _db->close();
    }


    void Database::deleteDatabase() {
        mustNotBeInTransaction();
        WITH_LOCK(this);
        if (refCount() > 1)
            error::_throw(error::Busy);
        if (config.flags & kC4DB_Bundled) {
            FilePath bundle = path().dir();
            _db->close();
            bundle.delRecursive();
        } else {
            _db->deleteDataFile();
        }
    }


    /*static*/ void Database::deleteDatabaseAtPath(const string &dbPath,
                                                   const C4DatabaseConfig *config) {
        if (config == nullptr) {
            FilePath(dbPath).delWithAllExtensions();
        } else if (config->flags & kC4DB_Bundled) {
            FilePath bundle{dbPath, ""};
            bundle.delRecursive();
        } else {
            FilePath path(dbPath);
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
    }


    void Database::compact() {
        mustNotBeInTransaction();
        WITH_LOCK(this);
        dataFile()->compact();
    }


    void Database::setOnCompact(DataFile::OnCompactCallback callback) noexcept {
        WITH_LOCK(this);
        dataFile()->setOnCompact(callback);
    }


    void Database::rekey(const C4EncryptionKey *newKey) {
        mustNotBeInTransaction();
        WITH_LOCK(this);
        rekeyDataFile(dataFile(), newKey);
    }


    /*static*/ void Database::rekeyDataFile(DataFile* database, const C4EncryptionKey *newKey)
    {
        if (newKey) {
            database->rekey((EncryptionAlgorithm)newKey->algorithm,
                            slice(newKey->bytes, 32));
        } else {
            database->rekey(kNoEncryption, nullslice);
        }
    }


#pragma mark - ACCESSORS:


    FilePath Database::path() const {
        FilePath path = _db->filePath();
        if (config.flags & kC4DB_Bundled)
            path = path.dir();
        return path;
    }


    uint64_t Database::countDocuments() {
        WITH_LOCK(this);
        RecordEnumerator::Options opts;
        opts.contentOptions = kMetaOnly;

        uint64_t count = 0;
        for (RecordEnumerator e(defaultKeyStore(), nullslice, nullslice, opts);
             e.next(); ) {
            C4DocumentFlags flags;
            if (documentFactory().readDocMeta(e.record(), &flags) && !(flags & kDeleted))
                ++count;
        }
        return count;
    }


    time_t Database::nextDocumentExpirationTime() {
        WITH_LOCK(this);
        KeyStore& expiryKvs = getKeyStore("expiry");
        RecordEnumerator e(expiryKvs);
        if(e.next() && e.record().body() == nullslice) {
            // Look for an entry with a null body (otherwise, its key is simply a doc ID)
            CollatableReader r(e.record().key());
            r.beginArray();
            return (time_t)r.readInt();
        }
        return 0;
    }


    KeyStore& Database::defaultKeyStore()                         {return _db->defaultKeyStore();}
    KeyStore& Database::getKeyStore(const string &name) const     {return _db->getKeyStore(name);}


#pragma mark - TRANSACTIONS:


    // NOTE: The lock order is always: first _transactionMutex, then _mutex.
    // The transaction methods below acquire _transactionMutex;
    // so do not call them if _mutex is already locked (after WITH_LOCK) or deadlock may occur!


    void Database::beginTransaction() {
    #if C4DB_THREADSAFE
        _transactionMutex.lock(); // this is a recursive mutex
    #endif
        if (++_transactionLevel == 1) {
            WITH_LOCK(this);
            _transaction = new Transaction(_db.get());
            _sequenceTracker->beginTransaction();
        }
    }

    bool Database::inTransaction() noexcept {
    #if C4DB_THREADSAFE
        lock_guard<recursive_mutex> lock(_transactionMutex);
    #endif
        return _transactionLevel > 0;
    }


    void Database::endTransaction(bool commit) {
    #if C4DB_THREADSAFE
        lock_guard<recursive_mutex> lock(_transactionMutex);
    #endif
        if (_transactionLevel == 0)
            error::_throw(error::NotInTransaction);
        if (--_transactionLevel == 0) {
            WITH_LOCK(this);
            auto t = _transaction;
            try {
                if (commit)
                    t->commit();
                else
                    t->abort();
            } catch (...) {
                delete t;
                _transaction = nullptr;
                _sequenceTracker->endTransaction(false);
                throw;
            }
            delete t;
            _transaction = nullptr;
            _sequenceTracker->endTransaction(commit);
        }
    #if C4DB_THREADSAFE
        _transactionMutex.unlock(); // undoes lock in beginTransaction()
    #endif
    }


    void Database::mustBeInTransaction() {
        if (!inTransaction())
            error::_throw(error::NotInTransaction);
    }

    void Database::mustNotBeInTransaction() {
        if (inTransaction())
            error::_throw(error::NotInTransaction);
    }


    Transaction& Database::transaction() const {
        auto t = _transaction;
        if (!t) error::_throw(error::NotInTransaction);
        return *t;
    }


#pragma mark - DOCUMENTS:

    
    bool Database::purgeDocument(slice docID) {
        WITH_LOCK(this);
        return defaultKeyStore().del(docID, transaction());
    }


    Record Database::getRawDocument(const string &storeName, slice key) {
        WITH_LOCK(this);
        return getKeyStore(storeName).get(key);
    }


    void Database::putRawDocument(const string &storeName, slice key, slice meta, slice body) {
        WITH_LOCK(this);
        KeyStore &localDocs = getKeyStore(storeName);
        auto &t = transaction();
        if (body.buf || meta.buf)
            localDocs.set(key, meta, body, t);
        else
            localDocs.del(key, t);
    }


    fleece::Encoder& Database::sharedEncoder() {
        WITH_LOCK(this);
        _encoder->reset();
        return *_encoder.get();
    }


    void Database::saved(Document* doc) {
        WITH_LOCK(this);
        _sequenceTracker->documentChanged(doc->docID, doc->sequence);
    }

}
