//
// DatabaseImpl.cc
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

#include "DatabaseImpl.hh"
#include "c4Collection.hh"
#include "c4Document.hh"
#include "c4Document.h"
#include "c4Internal.hh"
#include "c4Private.h"
#include "c4BlobStore.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "BackgroundDB.hh"
#include "Housekeeper.hh"
#include "DataFile.hh"
#include "SQLiteDataFile.hh"
#include "Record.hh"
#include "SequenceTracker.hh"
#include "FleeceImpl.hh"
#include "Upgrader.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include "PrebuiltCopier.hh"
#include <functional>
#include <inttypes.h>

namespace litecore::constants {
    const C4Slice kLocalCheckpointStore   = C4STR("checkpoints");
    const C4Slice kPeerCheckpointStore    = C4STR("peerCheckpoints");
    const C4Slice kPreviousPrivateUUIDKey = C4STR("previousPrivateUUID");
}

namespace litecore {
    using namespace fleece;
    using namespace fleece::impl;
    using namespace std;


    static const slice kMaxRevTreeDepthKey = "maxRevTreeDepth"_sl;
    static uint32_t kDefaultMaxRevTreeDepth = 20;

    const slice DatabaseImpl::kPublicUUIDKey = "publicUUID"_sl;
    const slice DatabaseImpl::kPrivateUUIDKey = "privateUUID"_sl;


    static string collectionNameToKeyStoreName(slice collectionName);
    static slice keyStoreNameToCollectionName(slice name);


#pragma mark - LIFECYCLE:


    Retained<DatabaseImpl> DatabaseImpl::open(const FilePath &path, C4DatabaseConfig config) {
        Retained<DatabaseImpl> db = new DatabaseImpl(path, config);
        db->open(path);
        return db;
    }


    DatabaseImpl::DatabaseImpl(const FilePath &path, C4DatabaseConfig inConfig)
    :_name(path.unextendedName())
    ,_parentDirectory(path.parentDir())
    ,_config{slice(_parentDirectory), inConfig.flags, inConfig.encryptionKey}
    ,_configV1(inConfig)
    ,_encoder(new fleece::impl::Encoder())
    { }


    // `path` is path to bundle; return value is path to db file. Updates config.storageEngine. */
    /*static*/ FilePath DatabaseImpl::findOrCreateBundle(const string &path,
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


    void DatabaseImpl::open(const FilePath &bundlePath) {
        FilePath dataFilePath = findOrCreateBundle(bundlePath,
                                                   (_configV1.flags & kC4DB_Create) != 0,
                                                   _configV1.storageEngine);
        // Set up DataFile options:
        DataFile::Options options { };
        options.keyStores.sequences = true;
        options.create = (_config.flags & kC4DB_Create) != 0;
        options.writeable = (_config.flags & kC4DB_ReadOnly) == 0;
        options.upgradeable = (_config.flags & kC4DB_NoUpgrade) == 0;
        options.useDocumentKeys = true;
        options.encryptionAlgorithm = (EncryptionAlgorithm)_config.encryptionKey.algorithm;
        if (options.encryptionAlgorithm != kNoEncryption) {
#ifdef COUCHBASE_ENTERPRISE
            options.encryptionKey = alloc_slice(_config.encryptionKey.bytes,
                                                kEncryptionKeySize[options.encryptionAlgorithm]);
#else
            error::_throw(error::UnsupportedEncryption);
#endif
        }


        // Determine the storage type and its Factory object:
        const char *storageEngine = _configV1.storageEngine ? _configV1.storageEngine : "";
        DataFile::Factory *storageFactory = DataFile::factoryNamed((string)(storageEngine));
        if (!storageFactory)
            error::_throw(error::Unimplemented);

        // Open the DataFile:
        try {
            _dataFile.reset( storageFactory->openFile(dataFilePath, this, &options) );
        } catch (const error &x) {
            if (x.domain == error::LiteCore && x.code == error::DatabaseTooOld
                    && UpgradeDatabaseInPlace(dataFilePath.dir(), _configV1)) {
                // This is an old 1.x database; upgrade it in place, then open:
                _dataFile.reset( storageFactory->openFile(dataFilePath, this, &options) );
            } else {
                throw;
            }
        }

        if (options.useDocumentKeys)
            _encoder->setSharedKeys(documentKeys());

        // Validate or upgrade the database's document schema/versioning:
        _configV1.versioning = checkDocumentVersioning();

        if (_configV1.versioning == kC4VectorVersioning)
            _config.flags |= kC4DB_VersionVectors;
        else
            _config.flags &= ~kC4DB_VersionVectors;

        // Start document-expiration tasks for all Collections that need them:
        initCollections();
        startBackgroundTasks();
    }


    DatabaseImpl::~DatabaseImpl() {
        Assert(_transactionLevel == 0,
               "Database being destructed while in a transaction");

        for (auto &entry : _collections)
            entry.second->close();

        FLEncoder_Free(_flEncoder);
        // Eagerly close the data file to ensure that no other instances will
        // be trying to use me as a delegate (for example in externalTransactionCommitted)
        // after I'm already in an invalid state
        if (_dataFile)
            _dataFile->close();
    }


#pragma mark - HOUSEKEEPING:


    C4DocumentVersioning DatabaseImpl::checkDocumentVersioning() {
        //FIXME: This ought to be done _before_ the SQLite userVersion is updated
        // Compare existing versioning against runtime config:
        auto &info = _dataFile->getKeyStore(DataFile::kInfoKeyStoreName, KeyStore::noSequences);
        Record versDoc = info.get("versioning");
        auto curVersioning = C4DocumentVersioning(versDoc.bodyAsUInt());
        auto newVersioning = _configV1.versioning;
        if (versDoc.exists() && curVersioning >= newVersioning)
            return curVersioning;

        // Mismatch -- could be a race condition. Open a transaction and recheck:
        Transaction t(this);
        versDoc = info.get("versioning");
        curVersioning = C4DocumentVersioning(versDoc.bodyAsUInt());
        if (versDoc.exists() && curVersioning >= newVersioning)
            return curVersioning;

        // Yup, mismatch confirmed, so deal with it:
        if (versDoc.exists()) {
            // Existing db versioning does not match runtime config!
            upgradeDocumentVersioning(curVersioning, newVersioning, transaction());
        } else if (_config.flags & kC4DB_Create) {
            // First-time initialization:
            (void)generateUUID(kPublicUUIDKey);
            (void)generateUUID(kPrivateUUIDKey);
        } else {
            // Should never occur (existing db must have its versioning marked!)
            error::_throw(error::WrongFormat);
        }

        // Store new versioning:
        versDoc.setBodyAsUInt((uint64_t)newVersioning);
        info.setKV(versDoc, transaction());
        t.commit();
        return newVersioning;
    }


    void DatabaseImpl::close() {
        mustNotBeInTransaction();
        stopBackgroundTasks();
        _dataFile->close();
    }


    void DatabaseImpl::deleteDatabase() {
        mustNotBeInTransaction();
        stopBackgroundTasks();
        FilePath bundle = path().dir();
        _dataFile->deleteDataFile();
        bundle.delRecursive();
    }


    /*static*/ bool DatabaseImpl::deleteDatabaseAtPath(const string &dbPath) {
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

    bool DatabaseImpl::deleteDatabaseFileAtPath(const string &dbPath,
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

    void DatabaseImpl::rekey(const C4EncryptionKey *newKey) {
        _dataFile->_logInfo("Rekeying database...");
        C4EncryptionKey keyBuf {kC4EncryptionNone, {}};
        if (!newKey)
            newKey = &keyBuf;

        mustNotBeInTransaction();
        stopBackgroundTasks();

        // Create a new BlobStore and copy/rekey the blobs into it:
        path().subdirectoryNamed("Attachments_temp").delRecursive();
        auto &blobStore = getBlobStore();
        auto newStore = createBlobStore("Attachments_temp", *newKey);
        try {
            blobStore.copyBlobsTo(*newStore);

            // Rekey the database itself:
            dataFile()->rekey((EncryptionAlgorithm)newKey->algorithm,
                              slice(newKey->bytes, kEncryptionKeySize[newKey->algorithm]));
        } catch (...) {
            newStore->deleteStore();
            throw;
        }

        const_cast<C4DatabaseConfig2&>(_config).encryptionKey = *newKey;

        // Finally replace the old BlobStore with the new one:
        blobStore.replaceWith(*newStore);
        startBackgroundTasks();
        _dataFile->_logInfo("Finished rekeying database!");
    }


#pragma mark - ACCESSORS:


    // Callback that takes a base64 blob digest and returns the blob data
    alloc_slice DatabaseImpl::blobAccessor(const Dict *blobDict) const {
        return getBlobStore().getBlobData(FLDict(blobDict));
    }


    FilePath DatabaseImpl::path() const {
        return _dataFile->filePath().dir();
    }


    uint32_t DatabaseImpl::maxRevTreeDepth() {
        if (_maxRevTreeDepth == 0) {
            auto &info = getKeyStore(DataFile::kInfoKeyStoreName);
            _maxRevTreeDepth = (uint32_t)info.get(kMaxRevTreeDepthKey).bodyAsUInt();
            if (_maxRevTreeDepth == 0)
                _maxRevTreeDepth = kDefaultMaxRevTreeDepth;
        }
        return _maxRevTreeDepth;
    }

    void DatabaseImpl::setMaxRevTreeDepth(uint32_t depth) {
        if (depth == 0)
            depth = kDefaultMaxRevTreeDepth;
        KeyStore &info = getKeyStore(DataFile::kInfoKeyStoreName);
        Record rec = info.get(kMaxRevTreeDepthKey);
        if (depth != rec.bodyAsUInt()) {
            rec.setBodyAsUInt(depth);
            ExclusiveTransaction t(*_dataFile);
            info.setKV(rec, t);
            t.commit();
        }
        _maxRevTreeDepth = depth;
    }


    KeyStore& DatabaseImpl::defaultKeyStore() const {
        return _dataFile->defaultKeyStore();
    }

    
    KeyStore& DatabaseImpl::getKeyStore(const string &nm) const {
        return _dataFile->getKeyStore(nm, KeyStore::noSequences);
    }


    C4BlobStore& DatabaseImpl::getBlobStore() const {
        if (!_blobStore)
            _blobStore = createBlobStore("Attachments", _config.encryptionKey);
        return *_blobStore;
    }


    unique_ptr<C4BlobStore> DatabaseImpl::createBlobStore(const string &dirname,
                                                          C4EncryptionKey encryptionKey) const
    {
        return make_unique<C4BlobStore>(alloc_slice(path().subdirectoryNamed(dirname)),
                                        _config.flags, encryptionKey);
    }


#pragma mark - HOUSEKEEPING:


    void DatabaseImpl::maintenance(DataFile::MaintenanceType what) {
        mustNotBeInTransaction();
        dataFile()->maintenance(what);
        if (what == DataFile::kCompact)
            garbageCollectBlobs();
    }


    void DatabaseImpl::garbageCollectBlobs() {
        // Lock the database to avoid any other thread creating a new blob, since if it did
        // I might end up deleting it during the sweep phase (deleteAllExcept).
        mustNotBeInTransaction();
        ExclusiveTransaction t(dataFile());

        unordered_set<C4BlobKey> usedDigests;
        auto blobCallback = [&](FLDict blob) {
            if (auto key = C4Blob::keyFromDigestProperty(blob); key)
            usedDigests.insert(*key);
            return true;
        };

        forEachCollection([&](C4Collection *coll) {
            coll->findBlobReferences(blobCallback);
        });

        // Now delete all blobs that don't have one of the referenced keys:
        auto numDeleted = getBlobStore().deleteAllExcept(usedDigests);
        if (numDeleted > 0 || !usedDigests.empty()) {
            LogTo(DBLog, "    ...deleted %u blobs (%zu remaining)",
                  numDeleted, usedDigests.size());
        }
    }

    BackgroundDB* DatabaseImpl::backgroundDatabase() {
        if (!_backgroundDB)
            _backgroundDB.reset(new BackgroundDB(this));
        return _backgroundDB.get();
    }


    void DatabaseImpl::stopBackgroundTasks() {
        // We can't hold the _collectionsMutex while calling stopHousekeeping(), or a deadlock may
        // result. So first enumerate the collections, then make the calls:
        vector<Retained<C4Collection>> collections;
        {
            LOCK(_collectionsMutex);
            for (auto &entry : _collections)
                collections.emplace_back(entry.second);
        }
        for (auto &coll : collections)
            coll->stopHousekeeping();

        if (_backgroundDB)
            _backgroundDB->close();
    }


    void DatabaseImpl::startBackgroundTasks() {
        for (const string &name : _dataFile->allKeyStoreNames()) {
            if (slice collName = keyStoreNameToCollectionName(name); collName) {
                if (_dataFile->getKeyStore(name).nextExpiration() > 0) {
                    getCollection(collName)->startHousekeeping();
                }
            }
        }
    }


    C4Timestamp DatabaseImpl::nextDocExpiration() const {
        C4Timestamp minTime = 0;
        forEachCollection([&](C4Collection *coll) {
            auto time = coll->nextDocExpiration();
            if (time > minTime || minTime == 0)
                minTime = time;
        });
        return minTime;
    }


#pragma mark - UUIDS:


    bool DatabaseImpl::getUUIDIfExists(slice key, C4UUID &uuid) {
        auto &store = getKeyStore(string(kInfoStore));
        Record r = store.get(key);
        if (!r.exists() || r.body().size < sizeof(C4UUID))
            return false;
        uuid = *(C4UUID*)r.body().buf;
        return true;
    }

    // must be called within a transaction
    C4UUID DatabaseImpl::generateUUID(slice key, bool overwrite) {
        C4UUID uuid;
        if (overwrite || !getUUIDIfExists(key, uuid)) {
            auto &store = getKeyStore(string(kInfoStore));
            slice uuidSlice{&uuid, sizeof(uuid)};
            GenerateUUID(uuidSlice);
            store.setKV(key, uuidSlice, transaction());
        }
        return uuid;
    }

    C4UUID DatabaseImpl::getUUID(slice key) {
        C4UUID uuid;
        if (!getUUIDIfExists(key, uuid)) {
            Transaction t(this);
            uuid = generateUUID(key);
            t.commit();
        }
        return uuid;
    }
    
    void DatabaseImpl::resetUUIDs() {
        Transaction t(this);
        C4UUID previousPrivate = getUUID(kPrivateUUIDKey);
        auto &store = getKeyStore(string(kInfoStore));
        store.setKV(constants::kPreviousPrivateUUIDKey,
                    {&previousPrivate, sizeof(C4UUID)},
                    transaction());
        generateUUID(kPublicUUIDKey, true);
        generateUUID(kPrivateUUIDKey, true);
        t.commit();
    }


    uint64_t DatabaseImpl::myPeerID() const {
        if (_myPeerID == 0) {
            // Compute my peer ID from the first 64 bits of the public UUID.
            auto uuid = const_cast<DatabaseImpl*>(this)->getUUID(kPublicUUIDKey);
            memcpy(&_myPeerID, &uuid, sizeof(_myPeerID));
            _myPeerID = endian::dec64(_myPeerID);
            // Don't let it be zero:
            if (_myPeerID == 0)
                _myPeerID = 1;
        }
        return _myPeerID;
    }


#pragma mark - COLLECTIONS:


    static constexpr const char* kDefaultCollectionName = "_default";

    static constexpr slice kCollectionNameCharacterSet
                            = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890_-%";

    static constexpr const char* kCollectionKeyStorePrefix = "coll_";


    MUST_USE_RESULT
    static bool validateCollectionName(slice name) {
        // Enforce CBServer collection name restrictions:
        return name.size >= 1 && name.size <= 30
            && !name.findByteNotIn(kCollectionNameCharacterSet)
            && name[0] != '_' && name[0] != '%';
    }


    static string collectionNameToKeyStoreName(slice collectionName) {
        if (collectionName == kDefaultCollectionName) {
            return DataFile::kDefaultKeyStoreName;
        } else if (validateCollectionName(collectionName)) {
            // KeyStore name is "coll_" + name; SQLite table name will be "kv_coll_" + name
            string result = kCollectionKeyStorePrefix;
            result.append(collectionName);
            return result;
        } else {
            return {};
        }
    }


    static slice keyStoreNameToCollectionName(slice name) {
        if (name == DataFile::kDefaultKeyStoreName)
            return kDefaultCollectionName;
        else if (hasPrefix(name, kCollectionKeyStorePrefix)) {
            name.moveStart(strlen(kCollectionKeyStorePrefix));
            return name;
        } else {
            return nullslice;
        }
    }


    void DatabaseImpl::initCollections() {
        LOCK(_collectionsMutex);
        _defaultCollection = getCollection(kDefaultCollectionName);
    }


    bool DatabaseImpl::hasCollection(slice name) const {
        LOCK(_collectionsMutex);
        string keyStoreName = collectionNameToKeyStoreName(name);
        return !keyStoreName.empty()
            && (_collections.find(string(name)) != _collections.end()
                || _dataFile->keyStoreExists(keyStoreName));
    }


    Retained<C4Collection> DatabaseImpl::getCollection(slice name) const {
        LOCK(_collectionsMutex);
        string nameStr(name);
        if (auto i = _collections.find(nameStr); i != _collections.end()) {
            return i->second;
        } else {
            string keyStoreName = collectionNameToKeyStoreName(name);
            if (keyStoreName.empty())
                C4Error::raise(LiteCoreDomain, kC4ErrorInvalidParameter,
                               "Invalid collection name '%.*s'", SPLAT(name));
            // getKeyStore() will create it if it doesn't exist...
            KeyStore &store = _dataFile->getKeyStore(keyStoreName);
            auto collection = C4Collection::newCollection(const_cast<DatabaseImpl*>(this),
                                                          name, store);
            if (isInTransaction())
                collection->transactionBegan();
            _collections.insert({nameStr, collection});
            return collection;
        }
    }


    Retained<C4Collection> DatabaseImpl::createCollection(slice name) {
        return getCollection(name);
    }


    void DatabaseImpl::deleteCollection(slice name) {
        LOCK(_collectionsMutex);
        if (auto i = _collections.find(string(name)); i != _collections.end()) {
            i->second->close();
            _collections.erase(i);
        }
        _dataFile->deleteKeyStore(collectionNameToKeyStoreName(name));
    }


    void DatabaseImpl::forgetCollection(C4Collection* coll) { // only called by ~C4Collection
        LOCK(_collectionsMutex);
        for (auto i = _collections.begin(); i != _collections.end(); ++i) {
            if (i->second == coll) {
                _collections.erase(i);
                return;
            }
        }
        postcondition(false);
    }


    vector<string> DatabaseImpl::collectionNames() const {
        vector<string> names;
        for (const string &name : _dataFile->allKeyStoreNames()) {
             if (slice collName = keyStoreNameToCollectionName(name); collName)
                names.emplace_back(collName);
        }
        return names;
    }


    void DatabaseImpl::forEachCollection(const function_ref<void(C4Collection*)> &callback) const {
        for (const auto &name : collectionNames()) {
            callback(getCollection(name).get());
        }
    }


    void DatabaseImpl::forEachOpenCollection(const function_ref<void(C4Collection*)> &callback) const {
        LOCK(_collectionsMutex);
        for (auto &entry : _collections)
            callback(entry.second);
    }

    
#pragma mark - TRANSACTIONS:


    void DatabaseImpl::beginTransaction() {
        if (++_transactionLevel == 1) {
            _transaction = new ExclusiveTransaction(_dataFile.get());
            forEachOpenCollection([&](C4Collection *coll) {
                coll->transactionBegan();
            });
        }
    }

    bool DatabaseImpl::inTransaction() noexcept {
        return _transactionLevel > 0;
    }


    void DatabaseImpl::mustBeInTransaction() {
        if (!inTransaction())
            error::_throw(error::NotInTransaction);
    }

    bool DatabaseImpl::mustBeInTransaction(C4Error *outError) noexcept {
        if (inTransaction())
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotInTransaction, {}, outError);
        return false;
    }

    bool DatabaseImpl::mustNotBeInTransaction(C4Error *outError) noexcept {
        if (!inTransaction())
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorTransactionNotClosed, {}, outError);
        return false;
    }


    void DatabaseImpl::endTransaction(bool commit) {
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
    void DatabaseImpl::_cleanupTransaction(bool committed) {
        forEachOpenCollection([&](C4Collection *coll) {
            coll->transactionEnding(_transaction, committed);
        });
        delete _transaction;
        _transaction = nullptr;
    }


    void DatabaseImpl::externalTransactionCommitted(const SequenceTracker &srcTracker) {
        // CAREFUL: This may be called on an arbitrary thread
        LOCK(_collectionsMutex);
        forEachOpenCollection([&](C4Collection *coll) {
            if (slice(coll->keyStore().name()) == srcTracker.name())
                coll->externalTransactionCommitted(srcTracker);
        });
    }


    void DatabaseImpl::mustNotBeInTransaction() {
        if (inTransaction())
            error::_throw(error::TransactionNotClosed);
    }


    ExclusiveTransaction& DatabaseImpl::transaction() const {
        auto t = _transaction;
        if (!t) error::_throw(error::NotInTransaction);
        return *t;
    }


#pragma mark - DOCUMENTS:

    
    Record DatabaseImpl::getRawRecord(const string &storeName, slice key) {
        return getKeyStore(storeName).get(key);
    }


    void DatabaseImpl::putRawRecord(const string &storeName, slice key, slice meta, slice body) {
        KeyStore &localDocs = getKeyStore(storeName);
        auto &t = transaction();
        if (body.buf || meta.buf)
            localDocs.setKV(key, meta, body, t);
        else
            localDocs.del(key, t);
    }


    fleece::impl::Encoder& DatabaseImpl::sharedEncoder() const {
        _encoder->reset();
        return *_encoder.get();
    }


    FLEncoder DatabaseImpl::sharedFLEncoder() const {
        if (_flEncoder) {
            FLEncoder_Reset(_flEncoder);
        } else {
            _flEncoder = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true);
            FLEncoder_SetSharedKeys(_flEncoder, (FLSharedKeys)documentKeys());
        }
        return _flEncoder;
    }


    // Validate that all dictionary keys in this value behave correctly, i.e. the keys found
    // through iteration also work for element lookup. (This tests the fix for issue #156.)
    // In a debug build this scans the entire collection recursively, while release will stick to
    // the top level
    static void validateKeys(const Value *val, bool atRoot =true) {
        // CBL-862: Need to reject invalid top level keys, even in release
        switch (val->type()) {
#if DEBUG
            case kArray:
                for (Array::iterator j(val->asArray()); j; ++j)
                    validateKeys(j.value(), false);
                break;
#endif
            case kDict: {
                const Dict *d = val->asDict();
                for (Dict::iterator i(d); i; ++i) {
                    auto key = i.keyString();
                    if (!key.buf || d->get(key) != i.value())
                        error::_throw(error::CorruptRevisionData,
                                      "Document key is not properly encoded");
                    if (atRoot && (key == "_id"_sl || key == "_rev"_sl || key == "_deleted"_sl))
                        error::_throw(error::CorruptRevisionData,
                                      "Illegal top-level key `%.*s` in document", SPLAT(key));
#if DEBUG
                    if (i.key()->asString() && val->sharedKeys()->couldAdd(key))
                        error::_throw(error::CorruptRevisionData,
                                      "Key `%.*s` should have been shared-key encoded", SPLAT(key));
                    validateKeys(i.value(), false);
#endif
                }
                break;
            }
            default:
                break;
        }
    }


    void DatabaseImpl::validateRevisionBody(slice body) {
        if (body.size > 0) {
            Scope scope(body, documentKeys());
            const Value *v = Value::fromData(body);
            if (!v)
                error::_throw(error::CorruptRevisionData, "Revision body is not parseable as Fleece");
            const Dict *root = v->asDict();
            if (!root)
                error::_throw(error::CorruptRevisionData, "Revision body is not a Dict");
            if (root->sharedKeys() != documentKeys())
                error::_throw(error::CorruptRevisionData,
                              "Revision uses wrong SharedKeys %p (db's is %p)",
                              root->sharedKeys(), documentKeys());
            validateKeys(v);
        }
    }

}
