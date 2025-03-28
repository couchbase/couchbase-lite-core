//
// DatabaseImpl.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "c4Private.h"
#include "c4Database.hh"
#include "c4DocumentTypes.h"
#include "DataFile.hh"
#include "FilePath.hh"
#include "HybridClock.hh"
#include "SourceID.hh"
#include "fleece/function_ref.hh"
#include "fleece/slice.hh"
#include <mutex>
#include <unordered_map>

C4_ASSUME_NONNULL_BEGIN

namespace fleece::impl {
    class Dict;
    class Encoder;
    class SharedKeys;
    class Value;
}  // namespace fleece::impl

namespace litecore {
    class BackgroundDB;
    class BlobStore;
    class Housekeeper;
    class RevTreeRecord;
    class SequenceTracker;

    // Stores and keys for raw documents:
    namespace constants {
        extern const C4Slice kLocalCheckpointStore;
        extern const C4Slice kPeerCheckpointStore;
        extern const C4Slice kPreviousPrivateUUIDKey;
    }  // namespace constants

    /** The concrete subclass of C4Database that implements its functionality.
        It also has some internal methods used by other components of LiteCore. */
    class DatabaseImpl final
        : public C4Database
        , public DataFile::Delegate {
      public:
        static Retained<DatabaseImpl> open(const FilePath& path, C4DatabaseConfig config);

        FilePath filePath() const { return _dataFile->filePath().dir(); }

        DataFile* dataFile() { return _dataFile.get(); }

        const DataFile* dataFile() const { return _dataFile.get(); }

        KeyStore& defaultKeyStore() const { return _dataFile->defaultKeyStore(); }

        BackgroundDB* backgroundDatabase();

        fleece::impl::Encoder& sharedEncoder() const;

        HybridClock& versionClock() const { return _versionClock; }

        SourceID mySourceID() const;

        void resetUUIDs() override;

        ExclusiveTransaction& transaction() const;
        void                  mustBeInTransaction() const;
        void                  mustNotBeInTransaction() const;

        uint32_t maxRevTreeDepth();
        void     setMaxRevTreeDepth(uint32_t depth);

        void validateRevisionBody(slice body);

        void forAllCollections(const function_ref<void(C4Collection*)>&) const;
        void forAllOpenCollections(const function_ref<void(C4Collection*)>&) const;

        // C4Database API:

        void checkOpen() const override { _dataFile->checkOpen(); }

        void close() override;
        void closeAndDeleteFile() override;

        alloc_slice getPath() const override { return alloc_slice(filePath()); }

        alloc_slice getSourceID() const override;

        C4UUID getPublicUUID() const override { return getUUID(kPublicUUIDKey); }

        C4UUID getPrivateUUID() const override { return getUUID(kPrivateUUIDKey); }

        void          rekey(const C4EncryptionKey* C4NULLABLE newKey) override;
        void          maintenance(C4MaintenanceType) override;
        void          forEachScope(const ScopeCallback&) const override;
        void          forEachCollection(const CollectionSpecCallback&) const override;
        bool          hasCollection(CollectionSpec) const override;
        bool          hasScope(C4String) const override;
        C4Collection* getCollection(CollectionSpec) const override;
        C4Collection* createCollection(CollectionSpec) override;
        void          deleteCollection(CollectionSpec) override;
        void          beginTransaction() override;
        void          endTransaction(bool commit) override;
        bool          isInTransaction() const noexcept override;
        C4Timestamp   nextDocExpiration() const override;
        bool          getRawDocument(slice storeName, slice key,
                                     fleece::function_ref<void(C4RawDocument* C4NULLABLE)> callback) override;
        void          putRawDocument(slice storeName, const C4RawDocument&) override;
        FLEncoder     sharedFleeceEncoder() const override;
        FLEncoder     createFleeceEncoder() const override;
        FLSharedKeys  getFleeceSharedKeys() const override;
        alloc_slice   encodeJSON(slice jsonData) const override;
        C4BlobStore&  getBlobStore() const override;

        alloc_slice rawQuery(slice query) override { return dataFile()->rawQuery(query.asString()); }

        void lockClientMutex() noexcept override { _clientMutex.lock(); }

        void unlockClientMutex() noexcept override { _clientMutex.unlock(); }

        C4RemoteID  getRemoteDBID(slice remoteAddress, bool canCreate) override;
        alloc_slice getRemoteDBAddress(C4RemoteID remoteID) override;
        alloc_slice getRevIDGlobalForm(slice revID) override;

        // DataFile::Delegate API:

        string databaseName() const override { return _name; }

        alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        void        externalTransactionCommitted(const SequenceTracker&) override;
        void        collectionRemoved(const std::string& keyStoreName) override;

        C4DatabaseTag getDatabaseTag() const { return (C4DatabaseTag)_dataFile->databaseTag(); }

        void setDatabaseTag(C4DatabaseTag dbTag) { _dataFile->setDatabaseTag((DatabaseTag)dbTag); }

      private:
        friend struct C4Database;

        DatabaseImpl(const FilePath& dir, C4DatabaseConfig config);
        ~DatabaseImpl() override;

        void            open(const FilePath& path);
        void            initCollections();
        static FilePath findOrCreateBundle(const string& path, bool canCreate, const char* C4NONNULL& outStorageEngine);
        void            _cleanupTransaction(bool committed);
        bool            getUUIDIfExists(slice key, C4UUID&) const;
        C4UUID          generateUUID(slice key, bool overwrite = false);

        KeyStore& infoKeyStore() const;
        Record    getInfo(slice key) const;
        void      setInfo(slice key, slice body);
        void      setInfo(Record&);
        KeyStore& rawDocStore(slice storeName);

        C4UUID                 getUUID(slice key) const;
        static constexpr slice kPublicUUIDKey  = "publicUUID";
        static constexpr slice kPrivateUUIDKey = "privateUUID";

        void startBackgroundTasks();
        void stopBackgroundTasks();

        unique_ptr<C4BlobStore> createBlobStore(const std::string& dirname, C4EncryptionKey, bool force = false) const;
        void                    garbageCollectBlobs();

        C4Collection* getOrCreateCollection(CollectionSpec, bool canCreate);

        C4DocumentVersioning checkDocumentVersioning();

        using CollectionsMap = std::unordered_map<CollectionSpec, Retained<C4Collection>>;

        unique_ptr<DataFile>                      _dataFile;  // Underlying DataFile
        mutable std::recursive_mutex              _collectionsMutex;
        mutable CollectionsMap                    _collections;
        ExclusiveTransaction* C4NULLABLE          _transaction{nullptr};  // Current ExclusiveTransaction, or null
        int                                       _transactionLevel{0};   // Nesting level of transactions
        mutable unique_ptr<fleece::impl::Encoder> _encoder;               // Shared Fleece Encoder
        mutable FLEncoder C4NULLABLE              _flEncoder{nullptr};    // Ditto, for clients
        mutable unique_ptr<C4BlobStore>           _blobStore;             // Blob storage
        uint32_t                                  _maxRevTreeDepth{0};    // Max revision-tree depth
        std::recursive_mutex                      _clientMutex;           // Mutex for c4db_lock/unlock
        unique_ptr<BackgroundDB>                  _backgroundDB;          // for background operations
        mutable SourceID                          _mySourceID;            // My identifier in version vectors
        mutable HybridClock                       _versionClock;          // Version-vector clock
    };

    inline DatabaseImpl* asInternal(const C4Database* db) { return (DatabaseImpl*)db; }

}  // namespace litecore

C4_ASSUME_NONNULL_END
