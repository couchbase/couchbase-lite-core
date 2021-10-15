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

#include "c4Database.hh"
#include "c4DocumentTypes.h"
#include "DataFile.hh"
#include "FilePath.hh"
#include "function_ref.hh"
#include "fleece/slice.hh"
#include <mutex>
#include <unordered_map>

C4_ASSUME_NONNULL_BEGIN

namespace fleece { namespace impl {
    class Dict;
    class Encoder;
    class SharedKeys;
    class Value;
} }

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
    }

    /** The concrete subclass of C4Database that implements its functionality.
        It also has some internal methods used by other components of LiteCore. */
    class DatabaseImpl final : public C4Database,
                               public DataFile::Delegate
    {
    public:
        static Retained<DatabaseImpl> open(const FilePath &path, C4DatabaseConfig config);

        FilePath filePath() const                       {return _dataFile->filePath().dir();}

        DataFile* dataFile()                            {return _dataFile.get();}
        const DataFile* dataFile() const                {return _dataFile.get();}

        KeyStore& defaultKeyStore() const               {return _dataFile->defaultKeyStore();}

        BackgroundDB* backgroundDatabase();

        fleece::impl::Encoder& sharedEncoder() const;

        uint64_t myPeerID() const;

        void resetUUIDs();

        ExclusiveTransaction& transaction() const;
        void mustBeInTransaction();
        void mustNotBeInTransaction();

        uint32_t maxRevTreeDepth();
        void setMaxRevTreeDepth(uint32_t depth);

        void validateRevisionBody(slice body);


        // C4Database API:

        void close() override;
        void closeAndDeleteFile() override;
        alloc_slice getPath() const override               {return filePath();}
        alloc_slice getPeerID() const override;
        C4UUID getPublicUUID() const override              {return getUUID(kPublicUUIDKey);}
        C4UUID getPrivateUUID() const override             {return getUUID(kPrivateUUIDKey);}
        void rekey(const C4EncryptionKey* C4NULLABLE newKey) override;
        void maintenance(C4MaintenanceType) override;
        std::vector<std::string> getCollectionNames() const override;
        void forEachCollection(const CollectionCallback&) const override;
        void forEachOpenCollection(const CollectionCallback&) const;
        bool hasCollection(slice name) const override;
        C4Collection* getCollection(slice name) const override;
        C4Collection* createCollection(slice name) override;
        void deleteCollection(slice name) override;
        void beginTransaction() override;
        void endTransaction(bool commit) override;
        bool isInTransaction() const noexcept override;
        C4Timestamp nextDocExpiration() const override;
        bool getRawDocument(slice storeName,
                            slice key,
                            fleece::function_ref<void(C4RawDocument* C4NULLABLE)> callback) override;
        void putRawDocument(slice storeName, const C4RawDocument&) override;
        FLEncoder sharedFleeceEncoder() const override;
        FLEncoder createFleeceEncoder() const override;
        FLSharedKeys getFleeceSharedKeys() const override;
        alloc_slice encodeJSON(slice jsonData) const override;
        C4BlobStore& getBlobStore() const override;
        alloc_slice rawQuery(slice query) override {return dataFile()->rawQuery(query.asString());}
        void lockClientMutex() noexcept override                         {_clientMutex.lock();}
        void unlockClientMutex() noexcept override                       {_clientMutex.unlock();}
        C4RemoteID getRemoteDBID(slice remoteAddress, bool canCreate) override;
        alloc_slice getRemoteDBAddress(C4RemoteID remoteID) override;

        // DataFile::Delegate API:

        virtual string databaseName() const override                    {return _name;}
        virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        virtual void externalTransactionCommitted(const SequenceTracker*) override;

    private:
        friend struct C4Database;

        DatabaseImpl(const FilePath &dir, C4DatabaseConfig config);
        virtual ~DatabaseImpl();

        void open(const FilePath &path);
        void initCollections();
        static FilePath findOrCreateBundle(const string &path, bool canCreate,
                                           const char * C4NONNULL &outStorageEngine);
        void _cleanupTransaction(bool committed);
        bool getUUIDIfExists(slice key, C4UUID&) const;
        C4UUID generateUUID(slice key, bool overwrite =false);

        KeyStore& infoKeyStore() const;
        Record getInfo(slice key) const;
        void setInfo(slice key, slice body);
        void setInfo(Record&);
        KeyStore& rawDocStore(slice storeName);

        C4UUID getUUID(slice key) const;
        static constexpr slice kPublicUUIDKey = "publicUUID";
        static constexpr slice kPrivateUUIDKey = "privateUUID";

        void startBackgroundTasks();
        void stopBackgroundTasks();

        unique_ptr<C4BlobStore> createBlobStore(const std::string &dirname, C4EncryptionKey) const;
        void garbageCollectBlobs();

        C4Collection* getOrCreateCollection(slice name, bool canCreate);

        C4DocumentVersioning checkDocumentVersioning();
        void upgradeDocumentVersioning(C4DocumentVersioning old, C4DocumentVersioning nuu,
                                       ExclusiveTransaction&);
        alloc_slice upgradeRemoteRevsToVersionVectors(RevTreeRecord&, alloc_slice currentVersion);

        using CollectionsMap = std::unordered_map<slice,std::unique_ptr<C4Collection>>;

        unique_ptr<DataFile>        _dataFile;              // Underlying DataFile
        mutable std::recursive_mutex _collectionsMutex;
        mutable CollectionsMap      _collections;
        ExclusiveTransaction* C4NULLABLE _transaction {nullptr}; // Current ExclusiveTransaction, or null
        int                         _transactionLevel {0};  // Nesting level of transactions
        mutable unique_ptr<fleece::impl::Encoder> _encoder; // Shared Fleece Encoder
        mutable FLEncoder C4NULLABLE _flEncoder {nullptr};  // Ditto, for clients
        mutable unique_ptr<C4BlobStore> _blobStore;         // Blob storage
        uint32_t                    _maxRevTreeDepth {0};   // Max revision-tree depth
        std::recursive_mutex        _clientMutex;           // Mutex for c4db_lock/unlock
        unique_ptr<BackgroundDB>    _backgroundDB;          // for background operations
        mutable uint64_t            _myPeerID {0};          // My identifier in version vectors
    };


    static inline DatabaseImpl* asInternal(const C4Database *db) {return (DatabaseImpl*)db;}

}

C4_ASSUME_NONNULL_END
