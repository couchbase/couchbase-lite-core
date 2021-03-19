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
#include "c4Document.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "c4Document.h"
#include "c4Private.h"
#include "c4BlobStore.hh"
#include "BackgroundDB.hh"
#include "Housekeeper.hh"
#include "DataFile.hh"
#include "SQLiteDataFile.hh"
#include "Record.hh"
#include "SequenceTracker.hh"
#include "FleeceImpl.hh"
#include "BlobStore.hh"
#include "Upgrader.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include "PrebuiltCopier.hh"
#include <functional>

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


#pragma mark - LIFECYCLE:


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


    DatabaseImpl::DatabaseImpl(const string &bundlePath,
                               C4DatabaseConfig inConfig)
    :DatabaseImpl(bundlePath,
                  inConfig,
                  findOrCreateBundle(bundlePath,
                                     (inConfig.flags & kC4DB_Create) != 0,
                                     inConfig.storageEngine))
    { }

    
    DatabaseImpl::DatabaseImpl(const string &bundlePath,
                               const C4DatabaseConfig &inConfig,
                               FilePath &&dataFilePath)
    :_name(dataFilePath.dir().unextendedName())
    ,_parentDirectory(dataFilePath.dir().parentDir())
    ,_config{slice(_parentDirectory), inConfig.flags, inConfig.encryptionKey}
    ,_configV1(inConfig)
    ,_encoder(new fleece::impl::Encoder())
    {
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
        const char *storageEngine = inConfig.storageEngine ? inConfig.storageEngine : "";
        DataFile::Factory *storageFactory = DataFile::factoryNamed((string)(storageEngine));
        if (!storageFactory)
            error::_throw(error::Unimplemented);

        // Open the DataFile:
        try {
            _dataFile.reset( storageFactory->openFile(dataFilePath, this, &options) );
        } catch (const error &x) {
            if (x.domain == error::LiteCore && x.code == error::DatabaseTooOld
                    && UpgradeDatabaseInPlace(dataFilePath.dir(), inConfig)) {
                // This is an old 1.x database; upgrade it in place, then open:
                _dataFile.reset( storageFactory->openFile(dataFilePath, this, &options) );
            } else {
                throw;
            }
        }

        if (options.useDocumentKeys)
            _encoder->setSharedKeys(documentKeys());

        if (!(_config.flags & kC4DB_NonObservable))
            _sequenceTracker.reset(new access_lock<SequenceTracker>());

        // Validate or upgrade the database's document schema/versioning:
        _configV1.versioning = checkDocumentVersioning();

        if (_configV1.versioning == kC4VectorVersioning) {
            _config.flags |= kC4DB_VersionVectors;
            _documentFactory = make_unique<VectorDocumentFactory>(this);
        } else {
            _config.flags &= ~kC4DB_VersionVectors;
            _documentFactory = make_unique<TreeDocumentFactory>(this);
        }
    }


    DatabaseImpl::~DatabaseImpl() {
        Assert(_transactionLevel == 0,
               "Database being destructed while in a transaction");
        FLEncoder_Free(_flEncoder);
        // Eagerly close the data file to ensure that no other instances will
        // be trying to use me as a delegate (for example in externalTransactionCommitted)
        // after I'm already in an invalid state
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

    unordered_set<string> DatabaseImpl::collectBlobs() {
        RecordEnumerator::Options options;
        options.onlyBlobs = true;
        options.sortOption = kUnsorted;
        RecordEnumerator e(defaultKeyStore(), options);
        unordered_set<string> usedDigests;
        while (e.next()) {
            Retained<C4Document> doc = documentFactory().newDocumentInstance(*e);
            doc->selectCurrentRevision();
            do {
                if(!doc->loadRevisionBody()) {
                    continue;
                }

                auto body = (const Dict*)doc->getProperties();

                // Iterate over blobs:
                C4Blob::findBlobReferences(FLDict(body), [&](FLDict blob) {
                    blobKey key;
                    if (C4Blob::isBlob(blob, (C4BlobKey&)key))    // get the key
                        usedDigests.insert(key.filename());
                    return true;
                });

                // Now look for old-style _attachments:
                auto attachments = body->get(C4Blob::kLegacyAttachmentsProperty);
                if (attachments) {
                    blobKey key;
                    for (Dict::iterator i(attachments->asDict()); i; ++i) {
                        auto att = i.value()->asDict();
                        if (att) {
                            const Value* digest = att->get(C4Blob::kDigestProperty);
                            if (digest && key.readFromBase64(digest->asString())) {
                                usedDigests.insert(key.filename());
                            }
                        }
                    }
                }
            } while(doc->selectNextRevision());
        }
        
        return usedDigests;
    }

    void DatabaseImpl::maintenance(DataFile::MaintenanceType what) {
        mustNotBeInTransaction();
        dataFile()->maintenance(what);

        if (what == DataFile::kCompact) {
            // After DB compaction, garbage-collect blobs:
            unordered_set<string> digestsInUse = collectBlobs();
            getBlobStore()._impl->deleteAllExcept(digestsInUse);
        }
    }


    void DatabaseImpl::rekey(const C4EncryptionKey *newKey) {
        _dataFile->_logInfo("Rekeying database...");
        C4EncryptionKey keyBuf {kC4EncryptionNone, {}};
        if (!newKey)
            newKey = &keyBuf;

        mustNotBeInTransaction();
        bool housekeeping = (_housekeeper != nullptr);
        stopBackgroundTasks();

        // Create a new BlobStore and copy/rekey the blobs into it:
        BlobStore *blobStoreImpl = getBlobStore()._impl.get();
        path().subdirectoryNamed("Attachments_temp").delRecursive();
        auto newStore = createBlobStore("Attachments_temp", *newKey);
        try {
            blobStoreImpl->copyBlobsTo(*newStore);

            // Rekey the database itself:
            dataFile()->rekey((EncryptionAlgorithm)newKey->algorithm,
                              slice(newKey->bytes, kEncryptionKeySize[newKey->algorithm]));
        } catch (...) {
            newStore->deleteStore();
            throw;
        }

        const_cast<C4DatabaseConfig2&>(_config).encryptionKey = *newKey;

        // Finally replace the old BlobStore with the new one:
        newStore->moveTo(*blobStoreImpl);
        if (housekeeping)
            startHousekeeping();
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


    uint64_t DatabaseImpl::countDocuments() {
        return defaultKeyStore().recordCount();
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
            _blobStore.reset(new C4BlobStore(createBlobStore("Attachments",
                                                             _config.encryptionKey)));
        return *_blobStore;
    }


    unique_ptr<BlobStore> DatabaseImpl::createBlobStore(const string &dirname,
                                                        C4EncryptionKey encryptionKey) const
    {
        FilePath blobStorePath = path().subdirectoryNamed(dirname);
        auto options = BlobStore::Options::defaults;
        options.create = options.writeable = (_config.flags & kC4DB_ReadOnly) == 0;
        options.encryptionAlgorithm =(EncryptionAlgorithm)encryptionKey.algorithm;
        if (options.encryptionAlgorithm != kNoEncryption) {
            options.encryptionKey = alloc_slice(encryptionKey.bytes, sizeof(encryptionKey.bytes));
        }
        return make_unique<BlobStore>(blobStorePath, &options);
    }


    access_lock<SequenceTracker>& DatabaseImpl::sequenceTracker() {
        if (!_sequenceTracker)
            error::_throw(error::UnsupportedOperation);
        return *_sequenceTracker;
    }


    BackgroundDB* DatabaseImpl::backgroundDatabase() {
        if (!_backgroundDB)
            _backgroundDB.reset(new BackgroundDB(this));
        return _backgroundDB.get();
    }


    void DatabaseImpl::stopBackgroundTasks() {
        if (_housekeeper) {
            _housekeeper->stop();
            _housekeeper = nullptr;
        }
        if (_backgroundDB)
            _backgroundDB->close();
    }


    bool DatabaseImpl::startHousekeeping() {
        if (!_housekeeper) {
            if (_config.flags & kC4DB_ReadOnly)
                return false;
            _housekeeper = new Housekeeper(this);
            _housekeeper->start();
        }
        return true;
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
    
    
#pragma mark - TRANSACTIONS:


    void DatabaseImpl::beginTransaction() {
        if (++_transactionLevel == 1) {
            _transaction = new ExclusiveTransaction(_dataFile.get());
            if (_sequenceTracker) {
                _sequenceTracker->use([](SequenceTracker &st) {
                    st.beginTransaction();
                });
            }
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
        if (_sequenceTracker) {
            _sequenceTracker->use([&](SequenceTracker &st) {
                if (committed && st.changedDuringTransaction()) {
                    // Notify other Database instances on this file:
                    _transaction->notifyCommitted(st);
                }
                st.endTransaction(committed);
            });
        }
        delete _transaction;
        _transaction = nullptr;
    }


    void DatabaseImpl::externalTransactionCommitted(const SequenceTracker &sourceTracker) {
        if (_sequenceTracker) {
            _sequenceTracker->use([&](SequenceTracker &st) {
                st.addExternalTransaction(sourceTracker);
            });
        }
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

    
    Retained<C4Document> DatabaseImpl::getDocument(slice docID,
                                                     bool mustExist,
                                                     C4DocContentLevel content) const
    {
        auto doc = documentFactory().newDocumentInstance(docID, ContentOption(content));
        if (mustExist && doc && !doc->exists())
            doc = nullptr;
        return doc;
    }


    C4Document* DatabaseImpl::documentContainingValue(FLValue value) noexcept {
        auto doc = VectorDocumentFactory::documentContaining(value);
        if (!doc)
            doc = TreeDocumentFactory::documentContaining(value);
        return doc;
    }


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


    void DatabaseImpl::documentSaved(C4Document* doc) {
        // CBL-1089
        // Conflicted documents are not eligible to be replicated,
        // so ignore them.  Later when the conflict is resolved
        // there will be logic to replicate them (see TreeDocument::resolveConflict)
        if (_sequenceTracker && !(doc->selectedRev().flags & kRevIsConflict)) {
            _sequenceTracker->use([doc](SequenceTracker &st) {
                Assert(doc->selectedRev().sequence == doc->sequence()); // The new revision must be selected
                st.documentChanged(doc->docID(),
                                   doc->getSelectedRevIDGlobalForm(), // entire version vector
                                   doc->selectedRev().sequence,
                                   SequenceTracker::RevisionFlags(doc->selectedRev().flags));
            });
        }
    }


    bool DatabaseImpl::purgeDocument(slice docID) {
        Transaction t(this);
        if (!defaultKeyStore().del(docID, transaction()))
            return false;
        if (_sequenceTracker) {
            _sequenceTracker->use([&](SequenceTracker &st) {
                st.documentPurged(docID);
            });
        }
        t.commit();
        return true;
    }


    int64_t DatabaseImpl::purgeExpiredDocs() {
        if (_sequenceTracker) {
            return _sequenceTracker->use<int64_t>([&](SequenceTracker &st) {
                return _dataFile->defaultKeyStore().expireRecords([&](slice docID) {
                    st.documentPurged(docID);
                });
            });
        } else {
            return _dataFile->defaultKeyStore().expireRecords();
        }
    }


    bool DatabaseImpl::setExpiration(slice docID, expiration_t expiration) {
        {
            Transaction t(this);
            if (!_dataFile->defaultKeyStore().setExpiration(docID, expiration))
                return false;
            t.commit();
        }
        if (_housekeeper)
            _housekeeper->documentExpirationChanged(expiration);
        return true;
    }

}
