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
#include "TreeDocument.hh"
#include "c4Internal.hh"
#include "c4Document.h"
#include "c4Document+Fleece.h"
#include "BackgroundDB.hh"
#include "Housekeeper.hh"
#include "DataFile.hh"
#include "Record.hh"
#include "SequenceTracker.hh"
#include "FleeceImpl.hh"
#include "BlobStore.hh"
#include "Upgrader.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include <functional>

namespace litecore { namespace constants
{
    const C4Slice kLocalCheckpointStore   = C4STR("checkpoints");
    const C4Slice kPeerCheckpointStore    = C4STR("peerCheckpoints");
    const C4Slice kPreviousPrivateUUIDKey = C4STR("previousPrivateUUID");
}}

namespace c4Internal {
    using namespace litecore;
    using namespace fleece;
    using namespace fleece::impl;
    using namespace std;


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


    Database::Database(const string &bundlePath,
                       C4DatabaseConfig inConfig,
                       C4DatabaseTag dbTag)
    :_dataFilePath(findOrCreateBundle(bundlePath,
                                      (inConfig.flags & kC4DB_Create) != 0,
                                      inConfig.storageEngine))
    ,config(inConfig)
    ,_encoder(new fleece::impl::Encoder())
    {
        // Set up DataFile options:
        DataFile::Options options { };
        options.keyStores.sequences = true;
        options.create = (config.flags & kC4DB_Create) != 0;
        options.writeable = (config.flags & kC4DB_ReadOnly) == 0;
        options.upgradeable = (config.flags & kC4DB_NoUpgrade) == 0;
        options.useDocumentKeys = true;
        options.encryptionAlgorithm = (EncryptionAlgorithm)config.encryptionKey.algorithm;
        options.dbTag = dbTag;
        if (options.encryptionAlgorithm != kNoEncryption) {
#ifdef COUCHBASE_ENTERPRISE
            options.encryptionKey = alloc_slice(config.encryptionKey.bytes,
                                                kEncryptionKeySize[options.encryptionAlgorithm]);
#else
            error::_throw(error::UnsupportedEncryption);
#endif
        }

        // Determine the storage type and its Factory object:
        const char *storageEngine = config.storageEngine ? config.storageEngine : "";
        DataFile::Factory *storageFactory = DataFile::factoryNamed((string)(storageEngine));
        if (!storageFactory)
            error::_throw(error::Unimplemented);

        // Initialize important objects:
        if (!(config.flags & kC4DB_NonObservable))
            _sequenceTracker.reset(new access_lock<SequenceTracker>());

        DocumentFactory* factory;
        switch (config.versioning) {
#if ENABLE_VERSION_VECTORS
            case kC4VersionVectors: factory = new VectorDocumentFactory(this); break;
#endif
            case kC4RevisionTrees:  factory = new TreeDocumentFactory(this); break;
            default:                error::_throw(error::InvalidParameter);
        }
        _documentFactory.reset(factory);

        // Open the DataFile:
        try {
            _dataFile.reset( storageFactory->openFile(_dataFilePath, this, &options) );
        } catch (const error &x) {
            if (x.domain == error::LiteCore && x.code == error::DatabaseTooOld
                    && UpgradeDatabaseInPlace(_dataFilePath.dir(), config)) {
                // This is an old 1.x database; upgrade it in place, then open:
                _dataFile.reset( storageFactory->openFile(_dataFilePath, this, &options) );
            } else {
                throw;
            }
        }

        if (options.useDocumentKeys)
            _encoder->setSharedKeys(documentKeys());

        // Validate that the versioning matches what's used in the database:
        auto &info = _dataFile->getKeyStore(DataFile::kInfoKeyStoreName);
        Record doc = info.get(slice("versioning"));
        if (doc.exists()) {
            if (doc.bodyAsUInt() != (uint64_t)config.versioning)
                error::_throw(error::WrongFormat);
        } else if (config.flags & kC4DB_Create) {
            // First-time initialization:
            doc.setBodyAsUInt((uint64_t)config.versioning);
            Transaction t(*_dataFile);
            info.write(doc, t);
            (void)generateUUID(kPublicUUIDKey, t);
            (void)generateUUID(kPrivateUUIDKey, t);
            t.commit();
        } else if (config.versioning != kC4RevisionTrees) {
            error::_throw(error::WrongFormat);
        }
    }


    Database::~Database() {
        Assert(_transactionLevel == 0,
               "Database being destructed while in a transaction");
        FLEncoder_Free(_flEncoder);
        // Eagerly close the data file to ensure that no other instances will
        // be trying to use me as a delegate (for example in externalTransactionCommitted)
        // after I'm already in an invalid state
        _dataFile->close();
    }


#pragma mark - HOUSEKEEPING:


    void Database::close() {
        mustNotBeInTransaction();
        stopBackgroundTasks();
        _dataFile->close();
    }


    void Database::deleteDatabase() {
        mustNotBeInTransaction();
        stopBackgroundTasks();
        FilePath bundle = path().dir();
        _dataFile->deleteDataFile();
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
        options.sortOption = kUnsorted;
        RecordEnumerator e(defaultKeyStore(), options);
        unordered_set<string> usedDigests;
        while (e.next()) {
            Retained<Document> doc = documentFactory().newDocumentInstance(*e);
            doc->selectCurrentRevision();
            do {
                if(!doc->loadSelectedRevBody()) {
                    continue;
                }

                Retained<Doc> fleeceDoc = doc->fleeceDoc();
                const Dict* body = fleeceDoc->asDict();

                // Iterate over blobs:
                Document::findBlobReferences(body, [&](const Dict *blob) {
                    blobKey key;
                    if (Document::dictIsBlob(blob, key))    // get the key
                        usedDigests.insert(key.filename());
                    return true;
                });

                // Now look for old-style _attachments:
                auto attachments = body->get(slice(kC4LegacyAttachmentsProperty));
                if (attachments) {
                    blobKey key;
                    for (Dict::iterator i(attachments->asDict()); i; ++i) {
                        auto att = i.value()->asDict();
                        if (att) {
                            const Value* digest = att->get(slice(kC4BlobDigestProperty));
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

    void Database::maintenance(DataFile::MaintenanceType what) {
        mustNotBeInTransaction();
        dataFile()->maintenance(what);

        if (what == DataFile::kCompact) {
            // After DB compaction, garbage-collect blobs:
            unordered_set<string> digestsInUse = collectBlobs();
            blobStore()->deleteAllExcept(digestsInUse);
        }
    }


    void Database::rekey(const C4EncryptionKey *newKey) {
        _dataFile->_logInfo("Rekeying database...");
        C4EncryptionKey keyBuf {kC4EncryptionNone, {}};
        if (!newKey)
            newKey = &keyBuf;

        mustNotBeInTransaction();
        bool housekeeping = (_housekeeper != nullptr);
        stopBackgroundTasks();

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

        const_cast<C4DatabaseConfig&>(config).encryptionKey = *newKey;

        // Finally replace the old BlobStore with the new one:
        newStore->moveTo(*realBlobStore);
        if (housekeeping)
            startHousekeeping();
        _dataFile->_logInfo("Finished rekeying database!");
    }


#pragma mark - ACCESSORS:


    slice Database::fleeceAccessor(slice recordBody) const {
        return TreeDocumentFactory::fleeceAccessor(recordBody);
    }


    // Callback that takes a base64 blob digest and returns the blob data
    alloc_slice Database::blobAccessor(const Dict *blobDict) const {
        return Document::getBlobData(blobDict, blobStore());
    }


    FilePath Database::path() const {
        return _dataFile->filePath().dir();
    }


    uint64_t Database::countDocuments() {
        return defaultKeyStore().recordCount();
    }


    uint32_t Database::maxRevTreeDepth() {
        if (_maxRevTreeDepth == 0) {
            auto &info = _dataFile->getKeyStore(DataFile::kInfoKeyStoreName);
            _maxRevTreeDepth = (uint32_t)info.get(kMaxRevTreeDepthKey).bodyAsUInt();
            if (_maxRevTreeDepth == 0)
                _maxRevTreeDepth = kDefaultMaxRevTreeDepth;
        }
        return _maxRevTreeDepth;
    }

    void Database::setMaxRevTreeDepth(uint32_t depth) {
        if (depth == 0)
            depth = kDefaultMaxRevTreeDepth;
        KeyStore &info = _dataFile->getKeyStore(DataFile::kInfoKeyStoreName);
        Record rec = info.get(kMaxRevTreeDepthKey);
        if (depth != rec.bodyAsUInt()) {
            rec.setBodyAsUInt(depth);
            Transaction t(*_dataFile);
            info.write(rec, t);
            t.commit();
        }
        _maxRevTreeDepth = depth;
    }


    KeyStore& Database::defaultKeyStore()                         {return _dataFile->defaultKeyStore();}
    KeyStore& Database::getKeyStore(const string &name) const     {return _dataFile->getKeyStore(name);}


    BlobStore* Database::blobStore() const {
        if (!_blobStore)
            _blobStore = createBlobStore("Attachments", config.encryptionKey);
        return _blobStore.get();
    }


    unique_ptr<BlobStore> Database::createBlobStore(const string &dirname,
                                                    C4EncryptionKey encryptionKey) const
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


    access_lock<SequenceTracker>& Database::sequenceTracker() {
        if (!_sequenceTracker)
            error::_throw(error::UnsupportedOperation);
        return *_sequenceTracker;
    }


    BackgroundDB* Database::backgroundDatabase() {
        if (!_backgroundDB)
            _backgroundDB.reset(new BackgroundDB(this));
        return _backgroundDB.get();
    }


    void Database::stopBackgroundTasks() {
        if (_housekeeper) {
            _housekeeper->stop();
            _housekeeper = nullptr;
        }
        if (_backgroundDB)
            _backgroundDB->close();
    }


    bool Database::startHousekeeping() {
        if (!_housekeeper) {
            if (config.flags & kC4DB_ReadOnly)
                return false;
            _housekeeper = new Housekeeper(this);
            _housekeeper->start();
        }
        return true;
    }


#pragma mark - UUIDS:


    bool Database::getUUIDIfExists(slice key, UUID &uuid) {
        auto &store = getKeyStore(toString(kC4InfoStore));
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
            auto &store = getKeyStore(toString(kC4InfoStore));
            slice uuidSlice{&uuid, sizeof(uuid)};
            GenerateUUID(uuidSlice);
            store.set(key, uuidSlice, t);
        }
        return uuid;
    }

    Database::UUID Database::getUUID(slice key) {
        UUID uuid;
        if (!getUUIDIfExists(key, uuid)) {
            TransactionHelper t(this);
            uuid = generateUUID(key, t);
            t.commit();
        }
        return uuid;
    }
    
    void Database::resetUUIDs() {
        TransactionHelper t(this);
        UUID previousPrivate = getUUID(kPrivateUUIDKey);
        auto &store = getKeyStore(toString(kC4InfoStore));
        store.set(constants::kPreviousPrivateUUIDKey, {&previousPrivate, sizeof(UUID)}, transaction());
        generateUUID(kPublicUUIDKey, t, true);
        generateUUID(kPrivateUUIDKey, t, true);
        t.commit();
    }
    
    
#pragma mark - TRANSACTIONS:


    void Database::beginTransaction() {
        if (++_transactionLevel == 1) {
            _transaction = new Transaction(_dataFile.get());
            if (_sequenceTracker) {
                _sequenceTracker->use([](SequenceTracker &st) {
                    st.beginTransaction();
                });
            }
        }
    }

    bool Database::inTransaction() noexcept {
        return _transactionLevel > 0;
    }


    bool Database::mustBeInTransaction(C4Error *outError) noexcept {
        if (inTransaction())
            return true;
        recordError(LiteCoreDomain, kC4ErrorNotInTransaction, outError);
        return false;
    }

    bool Database::mustNotBeInTransaction(C4Error *outError) noexcept {
        if (!inTransaction())
            return true;
        recordError(LiteCoreDomain, kC4ErrorTransactionNotClosed, outError);
        return false;
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
            _sequenceTracker->use([&](SequenceTracker &st) {
                if (committed) {
                    // Notify other Database instances on this file:
                    _transaction->notifyCommitted(st);
                }
                st.endTransaction(committed);
            });
        }
        delete _transaction;
        _transaction = nullptr;
    }


    void Database::externalTransactionCommitted(const SequenceTracker &sourceTracker) {
        if (_sequenceTracker) {
            _sequenceTracker->use([&](SequenceTracker &st) {
                st.addExternalTransaction(sourceTracker);
            });
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


    fleece::impl::Encoder& Database::sharedEncoder() {
        _encoder->reset();
        return *_encoder.get();
    }


    FLEncoder Database::sharedFLEncoder() {
        if (_flEncoder) {
            FLEncoder_Reset(_flEncoder);
        } else {
            _flEncoder = FLEncoder_NewWithOptions(kFLEncodeFleece, 512, true);
            FLEncoder_SetSharedKeys(_flEncoder, (FLSharedKeys)documentKeys());
        }
        return _flEncoder;
    }


#if DEBUG
    // Validate that all dictionary keys in this value behave correctly, i.e. the keys found
    // through iteration also work for element lookup. (This tests the fix for issue #156.)
    static void validateKeys(const Value *val, bool atRoot =true) {
        switch (val->type()) {
            case kArray:
                for (Array::iterator j(val->asArray()); j; ++j)
                    validateKeys(j.value(), false);
                break;
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
                    if (i.key()->asString() && val->sharedKeys()->couldAdd(key))
                        error::_throw(error::CorruptRevisionData,
                                      "Key `%.*s` should have been shared-key encoded", SPLAT(key));
                    validateKeys(i.value(), false);
                }
                break;
            }
            default:
                break;
        }
    }


    void Database::validateRevisionBody(slice body) {
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
#endif


    void Database::documentSaved(Document* doc) {
        // CBL-1089
        // Conflicted documents are not eligible to be replicated,
        // so ignore them.  Later when the conflict is resolved
        // there will be logic to replicate them (see TreeDocument::resolveConflict)
        if (_sequenceTracker && !(doc->selectedRev.flags & kRevIsConflict)) {
            _sequenceTracker->use([doc](SequenceTracker &st) {
                Assert(doc->selectedRev.sequence == doc->sequence); // The new revision must be selected
                st.documentChanged(doc->_docIDBuf,
                                   doc->_selectedRevIDBuf,
                                   doc->selectedRev.sequence,
                                   doc->selectedRev.body.size);
            });
        }
    }


    bool Database::purgeDocument(slice docID) {
        if (!defaultKeyStore().del(docID, transaction()))
            return false;
        if (_sequenceTracker) {
            _sequenceTracker->use([&](SequenceTracker &st) {
                st.documentPurged(docID);
            });
        }
        return true;
    }


    int64_t Database::purgeExpiredDocs() {
        if (_sequenceTracker) {
            return _sequenceTracker->use<int64_t>([&](SequenceTracker &st) {
                return _dataFile->defaultKeyStore().expireRecords([&](slice docID) {
                    st.documentPurged(docID);
                });
            });
        } else {
            return _dataFile->defaultKeyStore().expireRecords(nullptr);
        }
    }


    bool Database::setExpiration(slice docID, expiration_t expiration) {
        {
            TransactionHelper t(this);
            if (!_dataFile->defaultKeyStore().setExpiration(docID, expiration))
                return false;
            t.commit();
        }
        if (_housekeeper)
            _housekeeper->documentExpirationChanged(expiration);
        return true;
    }


#if 0 // unused
    bool Database::mustUseVersioning(C4DocumentVersioning requiredVersioning,
                                     C4Error *outError) noexcept
    {
        if (config.versioning == requiredVersioning)
            return true;
        recordError(LiteCoreDomain, kC4ErrorUnsupported, outError);
        return false;
    }
#endif


}
