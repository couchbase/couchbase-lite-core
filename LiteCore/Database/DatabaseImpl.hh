//
// DatabaseImpl.hh
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

#pragma once

#include "c4Database.hh"
#include "c4DocumentTypes.h"
#include "DataFile.hh"
#include "FilePath.hh"
#include "InstanceCounted.hh"
#include "access_lock.hh"
#include "function_ref.hh"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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


    /** The implementation of the C4Database class. */
    class DatabaseImpl final : public C4Database,
                               public DataFile::Delegate
    {
    public:
        static Retained<DatabaseImpl> open(const FilePath &path, C4DatabaseConfig config);

        void close();
        void deleteDatabase();
        static bool deleteDatabaseAtPath(const string &dbPath);

        DataFile* dataFile()                            {return _dataFile.get();}
        const DataFile* dataFile() const                {return _dataFile.get();}
        const string& name() const                      {return _name;}
        FilePath path() const;

        uint32_t maxRevTreeDepth();
        void setMaxRevTreeDepth(uint32_t depth);

        static const slice kPublicUUIDKey;
        static const slice kPrivateUUIDKey;

        C4UUID getUUID(slice key);
        void resetUUIDs();

        uint64_t myPeerID() const;

        void rekey(const C4EncryptionKey* C4NULLABLE newKey);
        
        void maintenance(DataFile::MaintenanceType what);

        const C4DatabaseConfig2* config() const         {return &_config;}
        const C4DatabaseConfig* configV1() const        {return &_configV1;};   // TODO: DEPRECATED

        std::vector<std::string> collectionNames() const;
        void forEachCollection(const function_ref<void(C4Collection*)>&) const;
        void forEachOpenCollection(const function_ref<void(C4Collection*)>&) const;
        bool hasCollection(slice name) const;
        Retained<C4Collection> getCollection(slice name) const;
        C4Collection* getDefaultCollection() const      {return _defaultCollection;}
        Retained<C4Collection> createCollection(slice name);
        void deleteCollection(slice name);

        void forgetCollection(C4Collection*); // only called by ~C4Collection

        ExclusiveTransaction& transaction() const;

        void beginTransaction();
        void endTransaction(bool commit);

        bool inTransaction() noexcept;
        void mustBeInTransaction();
        bool mustBeInTransaction(C4Error* C4NULLABLE outError) noexcept;
        bool mustNotBeInTransaction(C4Error* C4NULLABLE outError) noexcept;

        KeyStore& defaultKeyStore() const;
        KeyStore& getKeyStore(const string &name) const;

        C4Timestamp nextDocExpiration() const;

        void validateRevisionBody(slice body);

        Record getRawRecord(const std::string &storeName, slice key);
        void putRawRecord(const string &storeName, slice key, slice meta, slice body);

        fleece::impl::Encoder& sharedEncoder() const;
        FLEncoder sharedFLEncoder() const;

        fleece::impl::SharedKeys* documentKeys() const          {return _dataFile->documentKeys();}

        C4BlobStore& getBlobStore() const;

        void lockClientMutex() noexcept                         {_clientMutex.lock();}
        void unlockClientMutex() noexcept                       {_clientMutex.unlock();}

        // DataFile::Delegate API:
        virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        virtual void externalTransactionCommitted(const SequenceTracker&) override;

        BackgroundDB* backgroundDatabase();

    protected:
        friend class CollectionImpl;
        
        virtual ~DatabaseImpl();
        void mustNotBeInTransaction();

    private:
        DatabaseImpl(const FilePath &dir, C4DatabaseConfig config);
        void open(const FilePath &path);
        void initCollections();
        static FilePath findOrCreateBundle(const string &path, bool canCreate,
                                           const char * C4NONNULL &outStorageEngine);
        static bool deleteDatabaseFileAtPath(const string &dbPath, C4StorageEngine);
        void _cleanupTransaction(bool committed);
        bool getUUIDIfExists(slice key, C4UUID&);
        C4UUID generateUUID(slice key, bool overwrite =false);

        void startBackgroundTasks();
        void stopBackgroundTasks();

        unique_ptr<C4BlobStore> createBlobStore(const std::string &dirname, C4EncryptionKey) const;
        void garbageCollectBlobs();

        C4DocumentVersioning checkDocumentVersioning();
        void upgradeDocumentVersioning(C4DocumentVersioning old, C4DocumentVersioning nuu,
                                       ExclusiveTransaction&);
        alloc_slice upgradeRemoteRevsToVersionVectors(RevTreeRecord&, alloc_slice currentVersion);

        using CollectionsMap = std::unordered_map<std::string,Retained<C4Collection>>;

        string const                _name;                  // Database filename (w/o extension)
        string const                _parentDirectory;       // Path to parent directory
        C4DatabaseConfig2           _config;                // Configuration
        C4DatabaseConfig            _configV1;              // TODO: DEPRECATED
        unique_ptr<DataFile>        _dataFile;              // Underlying DataFile
        mutable std::recursive_mutex _collectionsMutex;
        mutable CollectionsMap      _collections;
        mutable Retained<C4Collection> _defaultCollection;
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

}

C4_ASSUME_NONNULL_END
