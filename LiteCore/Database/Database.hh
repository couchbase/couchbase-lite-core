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
#include <memory>
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

        DataFile* dataFile()                                {return _dataFile.get();}
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

        void rekey(const C4EncryptionKey *newKey);
        
        void maintenance(DataFile::MaintenanceType what);

        const C4DatabaseConfig config;

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

#if DEBUG
        void validateRevisionBody(slice body);
#else
        void validateRevisionBody(slice body)   { }
#endif

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
        virtual slice fleeceAccessor(slice recordBody) const override;
        virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const override;
        virtual void externalTransactionCommitted(const SequenceTracker&) override;

        BackgroundDB* backgroundDatabase();
        void stopBackgroundTasks();

        C4DatabaseTag getDatabaseTag() const {
            return (C4DatabaseTag)_dataFile->databaseTag();
        }

        void setDatabaseTag(C4DatabaseTag dbTag) {
            _dataFile->setDatabaseTag((DatabaseTag)dbTag);
        }

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
        static FilePath findOrCreateBundle(const string &path, bool canCreate,
                                           C4StorageEngine &outStorageEngine);
        static bool deleteDatabaseFileAtPath(const string &dbPath, C4StorageEngine);
        void _cleanupTransaction(bool committed);
        bool getUUIDIfExists(slice key, UUID&);
        UUID generateUUID(slice key, Transaction&, bool overwrite =false);

        std::unique_ptr<BlobStore> createBlobStore(const std::string &dirname, C4EncryptionKey) const;
        std::unordered_set<std::string> collectBlobs();
        void removeUnusedBlobs(const std::unordered_set<std::string> &used);

        FilePath                    _dataFilePath;          // Path of the DataFile
        std::unique_ptr<DataFile>        _dataFile;              // Underlying DataFile
        Transaction*                _transaction {nullptr}; // Current Transaction, or null
        int                         _transactionLevel {0};  // Nesting level of transaction
        std::unique_ptr<DocumentFactory> _documentFactory;       // Instantiates C4Documents
        std::unique_ptr<fleece::impl::Encoder> _encoder;         // Shared Fleece Encoder
        FLEncoder                   _flEncoder {nullptr};   // Ditto, for clients
        std::unique_ptr<access_lock<SequenceTracker>> _sequenceTracker; // Doc change tracker/notifier
        mutable std::unique_ptr<BlobStore> _blobStore;           // Blob storage
        uint32_t                    _maxRevTreeDepth {0};   // Max revision-tree depth
        std::recursive_mutex             _clientMutex;           // Mutex for c4db_lock/unlock
        std::unique_ptr<BackgroundDB>    _backgroundDB;          // for background operations
        Retained<Housekeeper>       _housekeeper;           // for expiration/cleanup tasks
    };

}

