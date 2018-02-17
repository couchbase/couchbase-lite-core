//
// Database.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Database.hh"
#include "Document.hh"
#include "c4Internal.hh"
#include "c4Document.h"
#include "DataFile.hh"
#include "Record.hh"
#include "SequenceTracker.hh"
#include "Fleece.hh"
#include "BlobStore.hh"
#include "Upgrader.hh"
#include "forestdb_endian.h"
#include "SecureRandomize.hh"
#include "make_unique.h"


namespace c4Internal {
    using namespace litecore;
    using namespace fleece;


    static const slice kMaxRevTreeDepthKey = "maxRevTreeDepth"_sl;
    static uint32_t kDefaultMaxRevTreeDepth = 20;

    const slice Database::kPublicUUIDKey = "publicUUID"_sl;
    const slice Database::kPrivateUUIDKey = "privateUUID"_sl;


#pragma mark - LIFECYCLE:


    // `path` is path to bundle; return value is path to db file. Updates config.storageEngine. */
    /*static*/ FilePath Database::findOrCreateBundle(const string &path,
                                                     bool canCreate,
                                                     C4StorageEngine &storageEngine)
    {
        FilePath bundle(path, "");
        bool createdDir = (canCreate && bundle.mkdir());
        if (!createdDir)
            bundle.mustExistAsDir();

        DataFile::Factory *factory = DataFile::factoryNamed(storageEngine);
        if (!factory)
            error::_throw(error::InvalidParameter);

        // Look for the file corresponding to the requested storage engine (defaulting to SQLite):

        FilePath dbPath = bundle["db"].withExtension(factory->filenameExtension());
        if (createdDir || factory->fileExists(dbPath)) {
            // Db exists in expected format, or else we just created this blank bundle dir, so exit:
            if (storageEngine == nullptr)
                storageEngine = factory->cname();
            return dbPath;
        }

        if (storageEngine != nullptr) {
            // DB exists but not in the format they specified, so fail:
            error::_throw(error::WrongFormat);
        }

        // Not found, but they didn't specify a format, so try the other formats:
        for (auto otherFactory : DataFile::factories()) {
            if (otherFactory != factory) {
                dbPath = bundle["db"].withExtension(otherFactory->filenameExtension());
                if (factory->fileExists(dbPath)) {
                    storageEngine = factory->cname();
                    return dbPath;
                }
            }
        }

        // Weird; the bundle exists but doesn't contain any known type of database, so fail:
        error::_throw(error::WrongFormat);
    }


    // subroutine of Database constructor that creates its _db
    /*static*/ DataFile* Database::newDataFile(const FilePath &path,
                                               const C4DatabaseConfig &config,
                                               bool isMainDB)
    {
        DataFile::Options options { };
        if (isMainDB) {
            options.keyStores.sequences = true;
        }
        options.create = (config.flags & kC4DB_Create) != 0;
        options.writeable = (config.flags & kC4DB_ReadOnly) == 0;
        options.useDocumentKeys = (config.flags & kC4DB_SharedKeys) != 0;

        options.encryptionAlgorithm = (EncryptionAlgorithm)config.encryptionKey.algorithm;
        if (options.encryptionAlgorithm != kNoEncryption) {
#ifdef COUCHBASE_ENTERPRISE
            options.encryptionKey = alloc_slice(config.encryptionKey.bytes,
                                                kEncryptionKeySize[options.encryptionAlgorithm]);
#else
            error::_throw(error::UnsupportedEncryption);
#endif
        }

        switch (config.versioning) {
            case kC4RevisionTrees:
                options.fleeceAccessor = TreeDocumentFactory::fleeceAccessor();
                break;
            default:
                error::_throw(error::InvalidParameter);
        }

        const char *storageEngine = config.storageEngine;
        if (!storageEngine) {
            storageEngine = "";
        }

        DataFile::Factory *storage = DataFile::factoryNamed((string)(storageEngine));
        if (!storage)
            error::_throw(error::Unimplemented);

        try {
            // Open the DataFile:
            return storage->openFile(path, &options);
        } catch (const error &x) {
            if (x.domain == error::LiteCore && x.code == error::DatabaseTooOld) {
                // This is an old 1.x database; upgrade it in place, then open:
                if (UpgradeDatabaseInPlace(path.dir(), config))
                    return storage->openFile(path, &options);
            }
            throw;
        }
    }


    Database::Database(const string &path,
                       C4DatabaseConfig inConfig)
    :_db(newDataFile(findOrCreateBundle(path,
                                        (inConfig.flags & kC4DB_Create) != 0,
                                        inConfig.storageEngine),
                     inConfig, true))
    ,config(inConfig)
    ,_encoder(new fleece::Encoder())
    {
        if (config.flags & kC4DB_SharedKeys)
            _encoder->setSharedKeys(documentKeys());
        if (!(config.flags & kC4DB_NonObservable))
            _sequenceTracker.reset(new SequenceTracker());

        // Validate that the versioning matches what's used in the database:
        auto &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
        Record doc = info.get(slice("versioning"));
        if (doc.exists()) {
            if (doc.bodyAsUInt() != (uint64_t)config.versioning)
                error::_throw(error::WrongFormat);
        } else if (config.flags & kC4DB_Create) {
            // First-time initialization:
            doc.setBodyAsUInt((uint64_t)config.versioning);
            Transaction t(*_db);
            info.write(doc, t);
            (void)generateUUID(kPublicUUIDKey, t);
            (void)generateUUID(kPrivateUUIDKey, t);
            t.commit();
        } else if (config.versioning != kC4RevisionTrees) {
            error::_throw(error::WrongFormat);
        }
        _db->setOwner(this);

        DocumentFactory* factory;
        switch (config.versioning) {
#if ENABLE_VERSION_VECTORS
            case kC4VersionVectors: factory = new VectorDocumentFactory(this); break;
#endif
            case kC4RevisionTrees:  factory = new TreeDocumentFactory(this); break;
            default:                error::_throw(error::InvalidParameter);
        }
        _documentFactory.reset(factory);
}


    Database::~Database() {
        Assert(_transactionLevel == 0);
    }


#pragma mark - HOUSEKEEPING:


    void Database::close() {
        mustNotBeInTransaction();
        _db->close();
    }


    void Database::deleteDatabase() {
        mustNotBeInTransaction();
        FilePath bundle = path().dir();
        _db->deleteDataFile();
        bundle.delRecursive();
    }


    /*static*/ bool Database::deleteDatabaseAtPath(const string &dbPath) {
        // Find the db file in the bundle:
        FilePath bundle {dbPath, ""};
        if (bundle.exists()) {
            try {
                C4StorageEngine storageEngine = nullptr;
                auto dbFilePath = findOrCreateBundle(dbPath, false, storageEngine);
                // Delete it:
                deleteDatabaseFileAtPath(dbFilePath, storageEngine);
            } catch (const error &x) {
                if (x.code != error::WrongFormat)   // ignore exception if db file isn't found
                    throw;
            }
        }
        // Delete the rest of the bundle:
        return bundle.delRecursive();
    }

    bool Database::deleteDatabaseFileAtPath(const string &dbPath,
                                            C4StorageEngine storageEngine) {
        FilePath path(dbPath);
        DataFile::Factory *factory = nullptr;
        if (storageEngine) {
            factory = DataFile::factoryNamed(storageEngine);
            if (!factory)
                Warn("c4db_deleteAtPath: unknown storage engine '%s'", storageEngine);
        } else {
            factory = DataFile::factoryForFile(path);
        }
        if (!factory)
            error::_throw(error::WrongFormat);
        return factory->deleteFile(path);
    }

    unordered_set<string> Database::collectBlobs() {
        RecordEnumerator::Options options;
        options.onlyBlobs = true;
        RecordEnumerator e(defaultKeyStore(), options);
        unordered_set<string> usedDigests;
        while (e.next()) {
            auto doc = documentFactory().newDocumentInstance(*e);
            doc->selectCurrentRevision();
            do {
                if(!doc->loadSelectedRevBody()) {
                    continue;
                }
                
                const Dict* body = Value::fromTrustedData(doc->selectedRev.body)->asDict();
                auto keys = _db->documentKeys();
                Document::findBlobReferencesAndKeys(body, keys,
                                                 [&usedDigests](const blobKey& key, uint64_t size) {
                    usedDigests.insert(key.filename());
                });
            } while(doc->selectNextRevision());
            
            delete doc;
        }
        
        return usedDigests;
    }

    void Database::compact() {
        mustNotBeInTransaction();
        dataFile()->compact();
        unordered_set<string> digestsInUse = collectBlobs();
        blobStore()->deleteAllExcept(digestsInUse);
    }


    void Database::rekey(const C4EncryptionKey *newKey) {
        LogTo(DBLog, "Rekeying database...");
        C4EncryptionKey keyBuf {kC4EncryptionNone, {}};
        if (!newKey)
            newKey = &keyBuf;

        mustNotBeInTransaction();

        // Create a new BlobStore and copy/rekey the blobs into it:
        BlobStore *realBlobStore = blobStore();
        path().subdirectoryNamed("Attachments_temp").delRecursive();
        auto newStore = createBlobStore("Attachments_temp", *newKey);
        try {
            realBlobStore->copyBlobsTo(*newStore);

            // Rekey the database itself:
            dataFile()->rekey((EncryptionAlgorithm)newKey->algorithm,
                              slice(newKey->bytes, kEncryptionKeySize[newKey->algorithm]));
        } catch (...) {
            newStore->deleteStore();
            throw;
        }

        ((C4DatabaseConfig&)config).encryptionKey = *newKey;

        // Finally replace the old BlobStore with the new one:
        newStore->moveTo(*realBlobStore);
        LogTo(DBLog, "Finished rekeying database!");
    }


#pragma mark - ACCESSORS:


    FilePath Database::path() const {
        return _db->filePath().dir();
    }


    uint64_t Database::countDocuments() {
        return defaultKeyStore().recordCount();
    }


    uint32_t Database::maxRevTreeDepth() {
        if (_maxRevTreeDepth == 0) {
            auto &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
            _maxRevTreeDepth = (uint32_t)info.get(kMaxRevTreeDepthKey).bodyAsUInt();
            if (_maxRevTreeDepth == 0)
                _maxRevTreeDepth = kDefaultMaxRevTreeDepth;
        }
        return _maxRevTreeDepth;
    }

    void Database::setMaxRevTreeDepth(uint32_t depth) {
        if (depth == 0)
            depth = kDefaultMaxRevTreeDepth;
        KeyStore &info = _db->getKeyStore(DataFile::kInfoKeyStoreName);
        Record rec = info.get(kMaxRevTreeDepthKey);
        if (depth != rec.bodyAsUInt()) {
            rec.setBodyAsUInt(depth);
            Transaction t(*_db);
            info.write(rec, t);
            t.commit();
        }
        _maxRevTreeDepth = depth;
    }


    KeyStore& Database::defaultKeyStore()                         {return _db->defaultKeyStore();}
    KeyStore& Database::getKeyStore(const string &name) const     {return _db->getKeyStore(name);}


    BlobStore* Database::blobStore() {
        if (!_blobStore)
            _blobStore = createBlobStore("Attachments", config.encryptionKey);
        return _blobStore.get();
    }


    unique_ptr<BlobStore> Database::createBlobStore(const string &dirname,
                                                    C4EncryptionKey encryptionKey)
    {
        FilePath blobStorePath = path().subdirectoryNamed(dirname);
        auto options = BlobStore::Options::defaults;
        options.create = options.writeable = (config.flags & kC4DB_ReadOnly) == 0;
        options.encryptionAlgorithm =(EncryptionAlgorithm)encryptionKey.algorithm;
        if (options.encryptionAlgorithm != kNoEncryption) {
            options.encryptionKey = alloc_slice(encryptionKey.bytes, sizeof(encryptionKey.bytes));
        }
        return make_unique<BlobStore>(blobStorePath, &options);
    }


    SequenceTracker& Database::sequenceTracker() {
        if (!_sequenceTracker)
            error::_throw(error::UnsupportedOperation);
        return *_sequenceTracker;
    }


#pragma mark - UUIDS:


    bool Database::getUUIDIfExists(slice key, UUID &uuid) {
        auto &store = getKeyStore((string)kC4InfoStore);
        Record r = store.get(key);
        if (!r.exists() || r.body().size < sizeof(UUID))
            return false;
        uuid = *(UUID*)r.body().buf;
        return true;
    }

    // must be called within a transaction
    Database::UUID Database::generateUUID(slice key, Transaction &t, bool overwrite) {
        UUID uuid;
        if (overwrite || !getUUIDIfExists(key, uuid)) {
            auto &store = getKeyStore((string)kC4InfoStore);
            slice uuidSlice{&uuid, sizeof(uuid)};
            GenerateUUID(uuidSlice);
            store.set(key, uuidSlice, t);
        }
        return uuid;
    }

    Database::UUID Database::getUUID(slice key) {
        UUID uuid;
        if (!getUUIDIfExists(key, uuid)) {
            beginTransaction();
            try {
                uuid = generateUUID(key, transaction());
            } catch (...) {
                endTransaction(false);
                throw;
            }
            endTransaction(true);
        }
        return uuid;
    }
    
    void Database::resetUUIDs() {
        beginTransaction();
        try {
            generateUUID(kPublicUUIDKey, transaction(), true);
            generateUUID(kPrivateUUIDKey, transaction(), true);
        } catch (...) {
            endTransaction(false);
            throw;
        }
        endTransaction(true);
    }
    
    
#pragma mark - TRANSACTIONS:


    // NOTE: The lock order is always: first _transactionMutex, then _mutex.
    // The transaction methods below acquire _transactionMutex;
    // so do not call them if _mutex is already locked (after WITH_LOCK) or deadlock may occur!


    void Database::beginTransaction() {
        if (++_transactionLevel == 1) {
            _transaction = new Transaction(_db.get());
            if (_sequenceTracker) {
                lock_guard<mutex> lock(_sequenceTracker->mutex());
                _sequenceTracker->beginTransaction();
            }
        }
    }

    bool Database::inTransaction() noexcept {
        return _transactionLevel > 0;
    }


    void Database::endTransaction(bool commit) {
        if (_transactionLevel == 0)
            error::_throw(error::NotInTransaction);
        if (--_transactionLevel == 0) {
            auto t = _transaction;
            try {
                if (commit)
                    t->commit();
                else
                    t->abort();
            } catch (...) {
                _cleanupTransaction(false);
                throw;
            }
            _cleanupTransaction(commit);
        }
    }


    // The cleanup part of endTransaction
    void Database::_cleanupTransaction(bool committed) {
        if (_sequenceTracker) {
            lock_guard<mutex> lock(_sequenceTracker->mutex());
            if (committed) {
                // Notify other Database instances on this file:
                _db->forOtherDataFiles([&](DataFile *other) {
                    auto otherDatabase = (Database*)other->owner();
                    if (otherDatabase)
                        otherDatabase->externalTransactionCommitted(*_sequenceTracker);
                });
            }
            _sequenceTracker->endTransaction(committed);
        }
        delete _transaction;
        _transaction = nullptr;
    }


    void Database::externalTransactionCommitted(const SequenceTracker &sourceTracker) {
        if (_sequenceTracker) {
            lock_guard<mutex> lock(_sequenceTracker->mutex());
            _sequenceTracker->addExternalTransaction(sourceTracker);
        }
    }


    void Database::mustNotBeInTransaction() {
        if (inTransaction())
            error::_throw(error::TransactionNotClosed);
    }


    Transaction& Database::transaction() const {
        auto t = _transaction;
        if (!t) error::_throw(error::NotInTransaction);
        return *t;
    }


#pragma mark - DOCUMENTS:

    
    bool Database::purgeDocument(slice docID) {
        return defaultKeyStore().del(docID, transaction());
    }


    Record Database::getRawDocument(const string &storeName, slice key) {
        return getKeyStore(storeName).get(key);
    }


    void Database::putRawDocument(const string &storeName, slice key, slice meta, slice body) {
        KeyStore &localDocs = getKeyStore(storeName);
        auto &t = transaction();
        if (body.buf || meta.buf)
            localDocs.set(key, meta, body, DocumentFlags::kNone, t);
        else
            localDocs.del(key, t);
    }


    fleece::Encoder& Database::sharedEncoder() {
        _encoder->reset();
        return *_encoder.get();
    }


#if DEBUG
    // Validate that all dictionary keys in this value behave correctly, i.e. the keys found
    // through iteration also work for element lookup. (This tests the fix for issue #156.)
    static void validateKeys(const Value *val, SharedKeys *sk) {
        switch (val->type()) {
            case kArray:
                for (Array::iterator j(val->asArray()); j; ++j)
                    validateKeys(j.value(), sk);
                break;
            case kDict: {
                const Dict *d = val->asDict();
                for (Dict::iterator i(d, sk); i; ++i) {
                    auto key = i.keyString();
                    if (!key.buf || d->get(key, sk) != i.value())
                        error::_throw(error::CorruptRevisionData,
                                      "Document key is not properly encoded");
                    validateKeys(i.value(), sk);
                }
                break;
            }
            default:
                break;
        }
    }


    void Database::validateRevisionBody(slice body) {
        if (body.size > 0 && body[0] != '{') {        // There are a few unit tests that store JSON
            const Value *v = Value::fromData(body);
            if (!v)
                error::_throw(error::CorruptRevisionData, "Revision body is not parseable as Fleece");
            const Dict *root = v->asDict();
            if (!root)
                error::_throw(error::CorruptRevisionData, "Revision body is not a Dict");
            validateKeys(v, documentKeys());
            for (Dict::iterator i(root, documentKeys()); i; ++i) {
                slice key = i.keyString();
                if (key == "_id"_sl || key == "_rev"_sl || key == "_deleted"_sl)
                    error::_throw(error::CorruptRevisionData, "Illegal key in document");
            }
        }
    }
#endif


    void Database::saved(Document* doc) {
        if (_sequenceTracker) {
            lock_guard<mutex> lock(_sequenceTracker->mutex());
            Assert(doc->selectedRev.sequence == doc->sequence); // The new revision must be selected
            _sequenceTracker->documentChanged(doc->_docIDBuf,
                                              doc->_selectedRevIDBuf,
                                              doc->selectedRev.sequence,
                                              doc->selectedRev.body.size);
        }
    }

}
