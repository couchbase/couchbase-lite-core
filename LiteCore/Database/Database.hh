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
#include "c4Private.h"
#include <memory>
#include <mutex>
#include <unordered_set>

namespace fleece {
    class Encoder;
    class SharedKeys;
}
namespace litecore {
    class SequenceTracker;
    class BlobStore;
}


namespace c4Internal {
    class Document;
    class DocumentFactory;


    /** A top-level LiteCore database. */
    class Database : public RefCounted, C4InstanceCounted {
    public:
        Database(const string &path, C4DatabaseConfig config);

        void close();
        void deleteDatabase();
        static bool deleteDatabaseAtPath(const string &dbPath);

        DataFile* dataFile()                                {return _db.get();}
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

        void compact();

        const C4DatabaseConfig config;

        Transaction& transaction() const;

        // Transaction methods below acquire _transactionMutex. Do not call them if
        // _mutex is already locked, or deadlock may occur!
        void beginTransaction();
        void endTransaction(bool commit);

        bool inTransaction() noexcept;

        KeyStore& defaultKeyStore();
        KeyStore& getKeyStore(const string &name) const;

        bool purgeDocument(slice docID);

#if DEBUG
        void validateRevisionBody(slice body);
#else
        void validateRevisionBody(slice body)   { }
#endif

        Record getRawDocument(const std::string &storeName, slice key);
        void putRawDocument(const string &storeName, slice key, slice meta, slice body);

        DocumentFactory& documentFactory()                  {return *_documentFactory;}

        fleece::Encoder& sharedEncoder();

        fleece::SharedKeys* documentKeys()                  {return _db->documentKeys();}

        SequenceTracker& sequenceTracker();

        BlobStore* blobStore();

        void lockClientMutex()                              {_clientMutex.lock();}
        void unlockClientMutex()                            {_clientMutex.unlock();}

    public:
        // should be private, but called from Document
        void saved(Document* NONNULL);

        // these should be private, but are also used by c4View
        static DataFile* newDataFile(const FilePath &path,
                                     const C4DatabaseConfig &config,
                                     bool isMainDB);
    protected:
        virtual ~Database();
        void mustNotBeInTransaction();
        void externalTransactionCommitted(const SequenceTracker&);

    private:
        static FilePath findOrCreateBundle(const string &path, bool canCreate,
                                           C4StorageEngine &outStorageEngine);
        static bool deleteDatabaseFileAtPath(const string &dbPath, C4StorageEngine);
        void _cleanupTransaction(bool committed);
        bool getUUIDIfExists(slice key, UUID&);
        UUID generateUUID(slice key, Transaction&, bool overwrite =false);

        std::unique_ptr<BlobStore> createBlobStore(const std::string &dirname, C4EncryptionKey);
        std::unordered_set<std::string> collectBlobs();
        void removeUnusedBlobs(const std::unordered_set<std::string> &used);

        unique_ptr<DataFile>        _db;                    // Underlying DataFile
        Transaction*                _transaction {nullptr}; // Current Transaction, or null
        int                         _transactionLevel {0};  // Nesting level of transaction
        unique_ptr<DocumentFactory> _documentFactory;       // Instantiates C4Documents
        unique_ptr<fleece::Encoder> _encoder;
        unique_ptr<SequenceTracker> _sequenceTracker;       // Doc change tracker/notifier
        unique_ptr<BlobStore>       _blobStore;
        uint32_t                    _maxRevTreeDepth {0};
        recursive_mutex             _clientMutex;
    };


    static inline C4Database* external(Database *db)    {return (C4Database*)db;}


    /** Abstract interface for creating Document instances; owned by a Database. */
    class DocumentFactory {
    public:
        DocumentFactory(Database *db)       :_db(db) { }
        Database* database() const          {return _db;}

        virtual ~DocumentFactory() { }
        virtual Document* newDocumentInstance(C4Slice docID) =0;
        virtual Document* newDocumentInstance(const Record&) =0;
        virtual alloc_slice revIDFromVersion(slice version) =0;
        virtual bool isFirstGenRevID(slice revID)               {return false;}

    private:
        Database* const _db;
    };


    /** DocumentFactory subclass for rev-tree document schema. */
    class TreeDocumentFactory : public DocumentFactory {
    public:
        TreeDocumentFactory(Database *db)   :DocumentFactory(db) { }
        Document* newDocumentInstance(C4Slice docID) override;
        Document* newDocumentInstance(const Record&) override;
        alloc_slice revIDFromVersion(slice version) override;
        bool isFirstGenRevID(slice revID) override;
        static DataFile::FleeceAccessor fleeceAccessor();
    };

}


// This is the struct that's forward-declared in the public c4Database.h
struct c4Database : public c4Internal::Database {
    c4Database(const FilePath &path, C4DatabaseConfig config)
    :Database(path, config) { }
    
    ~c4Database();
    
    FLEncoder sharedFLEncoder();

    bool mustUseVersioning(C4DocumentVersioning, C4Error*) noexcept;
    bool mustBeInTransaction(C4Error *outError) noexcept;
    bool mustNotBeInTransaction(C4Error *outError) noexcept;

private:
    FLEncoder                   _flEncoder {nullptr};
};

