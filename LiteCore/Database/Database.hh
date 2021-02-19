//
// Database.hh
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

#include "c4Internal.hh"
#include "c4Database.h"
#include "c4Document.h"
#include "DataFile.hh"
#include "FilePath.hh"
#include "InstanceCounted.hh"
#include "access_lock.hh"
#include <mutex>
#include <unordered_set>

namespace fleece { namespace impl {
    class Dict;
    class Encoder;
    class SharedKeys;
    class Value;
} }
namespace litecore {
    class SequenceTracker;
    class BlobStore;
    class BackgroundDB;
    class Housekeeper;
    class RevTreeRecord;
}


namespace c4Internal {
    class Document;
    class DocumentFactory;


    /** A top-level LiteCore database. */
    class Database : public RefCounted, public DataFile::Delegate, public fleece::InstanceCountedIn<Database> {
    public:
        Database(const string &path, C4DatabaseConfig config);

        void close();
        void deleteDatabase();
        static bool deleteDatabaseAtPath(const string &dbPath);

        DataFile* dataFile()                            {return _dataFile.get();}
        const string& name() const                      {return _name;}
        FilePath path() const;

        uint64_t countDocuments();
        sequence_t lastSequence()                       {return defaultKeyStore().lastSequence();}

        uint32_t maxRevTreeDepth();
        void setMaxRevTreeDepth(uint32_t depth);

        struct UUID {
            uint8_t bytes[16];
        };
        static const slice kPublicUUIDKey;
        static const slice kPrivateUUIDKey;

        UUID getUUID(slice key);
        void resetUUIDs();

        uint64_t myPeerID();

        void rekey(const C4EncryptionKey *newKey);
        
        void maintenance(DataFile::MaintenanceType what);

        const C4DatabaseConfig2* config() const         {return &_config;}
        const C4DatabaseConfig* configV1() const        {return &_configV1;};   // TODO: DEPRECATED

        Transaction& transaction() const;

        void beginTransaction();
        void endTransaction(bool commit);

        bool inTransaction() noexcept;
        bool mustBeInTransaction(C4Error *outError) noexcept;
        bool mustNotBeInTransaction(C4Error *outError) noexcept;

        class TransactionHelper {
        public:
            explicit TransactionHelper(Database* db)
            :_db(db)
            {
                db->beginTransaction();
            }

            void commit() {
                auto db = _db;
                _db = nullptr;
                db->endTransaction(true);
            }

            operator Transaction& () {
                return *_db->_transaction;
            }

            ~TransactionHelper() {
                if (_db)
                    _db->endTransaction(false);
            }

        private:
            Database* _db;
        };

        KeyStore& defaultKeyStore();
        KeyStore& getKeyStore(const string &name) const;

        bool purgeDocument(slice docID);
        int64_t purgeExpiredDocs();
        bool setExpiration(slice docID, expiration_t);
        bool startHousekeeping();

        void validateRevisionBody(slice body);

        Record getRawDocument(const std::string &storeName, slice key);
        void putRawDocument(const string &storeName, slice key, slice meta, slice body);

        DocumentFactory& documentFactory()                  {return *_documentFactory;}

        fleece::impl::Encoder& sharedEncoder();
        FLEncoder sharedFLEncoder();

        fleece::impl::SharedKeys* documentKeys()                  {return _dataFile->documentKeys();}

        access_lock<SequenceTracker>& sequenceTracker();

        BlobStore* blobStore() const;

        void lockClientMutex()                              {_clientMutex.lock();}
        void unlockClientMutex()                            {_clientMutex.unlock();}

        // DataFile::Delegate API:
        virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        virtual void externalTransactionCommitted(const SequenceTracker&) override;

        BackgroundDB* backgroundDatabase();
        void stopBackgroundTasks();

#if 0 // unused
        bool mustUseVersioning(C4DocumentVersioning, C4Error*) noexcept;
#endif

    public:
        // should be private, but called from Document
        void documentSaved(Document* NONNULL);

    protected:
        virtual ~Database();
        void mustNotBeInTransaction();

    private:
        Database(const string &bundlePath, const C4DatabaseConfig&, FilePath &&dataFilePath);
        static FilePath findOrCreateBundle(const string &path, bool canCreate,
                                           C4StorageEngine &outStorageEngine);
        static bool deleteDatabaseFileAtPath(const string &dbPath, C4StorageEngine);
        void _cleanupTransaction(bool committed);
        bool getUUIDIfExists(slice key, UUID&);
        UUID generateUUID(slice key, Transaction&, bool overwrite =false);

        unique_ptr<BlobStore> createBlobStore(const std::string &dirname, C4EncryptionKey) const;
        std::unordered_set<std::string> collectBlobs();
        void removeUnusedBlobs(const std::unordered_set<std::string> &used);

        C4DocumentVersioning checkDocumentVersioning();
        void upgradeDocumentVersioning(C4DocumentVersioning old, C4DocumentVersioning nuu,
                                       Transaction&);
        alloc_slice upgradeRemoteRevsToVersionVectors(RevTreeRecord&, alloc_slice currentVersion);

        const string                _name;                  // Database filename (w/o extension)
        const string                _parentDirectory;       // Path to parent directory
        C4DatabaseConfig2           _config;                // Configuration
        C4DatabaseConfig            _configV1;              // TODO: DEPRECATED
        unique_ptr<DataFile>        _dataFile;              // Underlying DataFile
        Transaction*                _transaction {nullptr}; // Current Transaction, or null
        int                         _transactionLevel {0};  // Nesting level of transaction
        unique_ptr<DocumentFactory> _documentFactory;       // Instantiates C4Documents
        unique_ptr<fleece::impl::Encoder> _encoder;         // Shared Fleece Encoder
        FLEncoder                   _flEncoder {nullptr};   // Ditto, for clients
        unique_ptr<access_lock<SequenceTracker>> _sequenceTracker; // Doc change tracker/notifier
        mutable unique_ptr<BlobStore> _blobStore;           // Blob storage
        uint32_t                    _maxRevTreeDepth {0};   // Max revision-tree depth
        std::recursive_mutex        _clientMutex;           // Mutex for c4db_lock/unlock
        unique_ptr<BackgroundDB>    _backgroundDB;          // for background operations
        Retained<Housekeeper>       _housekeeper;           // for expiration/cleanup tasks
        uint64_t                    _myPeerID {0};          // My identifier in version vectors
    };

}

