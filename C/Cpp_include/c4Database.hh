//
// c4Database.hh
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
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
#include "c4Base.hh"
#include "c4DatabaseTypes.h"
#include "c4DocumentTypes.h"
#include "c4IndexTypes.h"
#include "c4QueryTypes.h"
#include "function_ref.hh"
#include <functional>
#include <memory>
#include <vector>

struct C4ReplicatorParameters;

C4_ASSUME_NONNULL_BEGIN


// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/// Derives an encryption key from a user-entered password.
C4EncryptionKey C4EncryptionKeyFromPassword(fleece::slice password,
                                            C4EncryptionAlgorithm = kC4EncryptionAES256);


/// A LiteCore database connection.
struct C4Database : public fleece::RefCounted,
                    public fleece::InstanceCountedIn<C4Database>,
                    C4Base
{
public:
    // Lifecycle:

    using Config = C4DatabaseConfig2;

    static bool exists(slice name,
                       slice inDirectory);
    static void copyNamed(slice sourcePath,
                          slice destinationName,
                          const Config&);
    static bool deleteNamed(slice name,
                            slice inDirectory);
    static bool deleteAtPath(slice path);

    static Retained<C4Database> openNamed(slice name,
                                          const Config&);

    static Retained<C4Database> openAtPath(slice path,
                                           C4DatabaseFlags,
                                           const C4EncryptionKey* C4NULLABLE = nullptr);

    static void shutdownLiteCore();

    Retained<C4Database> openAgain()                    {return openNamed(getName(), getConfiguration());}

    virtual void close() =0;
    virtual void closeAndDeleteFile() =0;
    virtual void rekey(const C4EncryptionKey* C4NULLABLE key) =0;
    virtual void maintenance(C4MaintenanceType) =0;

    // Attributes:

    slice getName() const noexcept FLPURE                      {return _name;}
    virtual alloc_slice getPath() const =0;
    const Config& getConfiguration() const noexcept FLPURE     {return _config;}
    virtual alloc_slice getPeerID() const =0;
    virtual C4UUID getPublicUUID() const =0;
    virtual C4UUID getPrivateUUID() const =0;

    // Collections:

    /// Returns the default collection that exists in every database.
    /// In a pre-existing database, this collection contains all docs that were added to
    /// "the database" before collections existed.
    /// Its name is "_default".
    C4Collection* getDefaultCollection() const              {return _defaultCollection;}

    /// Returns true if the collection exists.
    virtual bool hasCollection(slice name) const =0;

    /// Returns the existing collection with the given name, or nullptr if it doesn't exist.
    virtual C4Collection* getCollection(slice name) const =0;

    /// Creates and returns an empty collection with the given name,
    /// or returns an existing collection by that name.
    virtual C4Collection* createCollection(slice name) =0;

    /// Deletes the collection with the given name.
    virtual void deleteCollection(slice name) =0;

    /// Returns the names of all existing collections, in the order in which they were created.
    virtual std::vector<std::string> getCollectionNames() const =0;

    using CollectionCallback = fleece::function_ref<void(C4Collection*)>;

    /// Calls the callback function for each collection, in the same order as collectionNames().
    virtual void forEachCollection(const CollectionCallback&) const =0;

#ifndef C4_STRICT_COLLECTION_API
    // Shims to ease the pain of converting to collections. These delegate to the default collection.
    uint64_t getDocumentCount() const;
    C4SequenceNumber getLastSequence() const;
    Retained<C4Document> getDocument(slice docID,
                                     bool mustExist = true,
                                     C4DocContentLevel content = kDocGetCurrentRev) const;
    Retained<C4Document> getDocumentBySequence(C4SequenceNumber sequence) const;
    Retained<C4Document> putDocument(const C4DocPutRequest &rq,
                                     size_t* C4NULLABLE outCommonAncestorIndex,
                                     C4Error *outError);
    bool purgeDocument(slice docID);
#endif

    // Transactions:

    /** Manages a transaction safely. The constructor begins a transaction, and \ref commit
        commits it. If the Transaction object exits scope without being committed, it aborts. */
    class Transaction {
    public:
        explicit Transaction(C4Database* db):_db(db) {db->beginTransaction();}
        Transaction(Transaction &&t)        :_db(t._db) {t._db = nullptr;}
        void commit()                       {auto db = _db; _db = nullptr; db->endTransaction(true);}
        void abort()                        {auto db = _db; _db = nullptr; db->endTransaction(false);}
        ~Transaction()                      {if (_db) _db->endTransaction(false);}
    private:
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        C4Database* C4NULLABLE _db;
    };

    virtual bool isInTransaction() const noexcept FLPURE =0;

    // Raw Documents:

    static constexpr slice kInfoStore = "info";    /// Raw-document store used for db metadata.

    virtual bool getRawDocument(slice storeName,
                                slice key,
                                fleece::function_ref<void(C4RawDocument* C4NULLABLE)> callback) =0;

    virtual void putRawDocument(slice storeName,
                                const C4RawDocument&) =0;

    // Fleece-related utilities for document encoding:

    virtual alloc_slice encodeJSON(slice jsonData) const =0;
    virtual FLEncoder createFleeceEncoder() const =0;
    virtual FLEncoder sharedFleeceEncoder() const =0;
    virtual FLSharedKeys getFleeceSharedKeys() const =0;

    // Expiration:

    virtual C4Timestamp nextDocExpiration() const =0;

    // Blobs:

    virtual C4BlobStore& getBlobStore() const =0;

    // Queries & Indexes:

    Retained<C4Query> newQuery(C4QueryLanguage language,
                               slice queryExpression,
                               int* C4NULLABLE outErrorPos = nullptr) const;

    virtual void createIndex(slice name,
                             slice indexSpecJSON,
                             C4IndexType indexType,
                             const C4IndexOptions* C4NULLABLE indexOptions =nullptr) =0;

    virtual void deleteIndex(slice name) =0;

    virtual alloc_slice getIndexesInfo(bool fullInfo = true) const =0;

    virtual alloc_slice getIndexRows(slice name) const =0;

    // Replicator:

    Retained<C4Replicator> newReplicator(C4Address serverAddress,
                                         slice remoteDatabaseName,
                                         const C4ReplicatorParameters &params);
    
    Retained<C4Replicator> newIncomingReplicator(C4Socket *openSocket,
                                                 const C4ReplicatorParameters &params);
    Retained<C4Replicator> newIncomingReplicator(litecore::websocket::WebSocket *openSocket,
                                                 const C4ReplicatorParameters &params);

#ifdef COUCHBASE_ENTERPRISE
    Retained<C4Replicator> newLocalReplicator(C4Database *otherLocalDB,
                                              const C4ReplicatorParameters &params);
#endif

    alloc_slice getCookies(const C4Address&);

    bool setCookie(slice setCookieHeader,
                   slice fromHost,
                   slice fromPath);

    void clearCookies();

// only used internally:
    // These are used by the replicator:
    virtual C4RemoteID getRemoteDBID(slice remoteAddress, bool canCreate) =0;
    virtual alloc_slice getRemoteDBAddress(C4RemoteID remoteID) =0;

    // Used only by the `cblite` tool:
    virtual alloc_slice rawQuery(slice sqliteQuery) =0;

    // Only for use by the C API -- internal or deprecated:
    virtual void beginTransaction() =0;            // use Transaction class above instead
    virtual void endTransaction(bool commit) =0;
    static void copyFileToPath(slice sourcePath, slice destinationPath, const C4DatabaseConfig&);
    const C4DatabaseConfig& configV1() const noexcept FLPURE {return _configV1;}
    virtual void lockClientMutex() noexcept =0;
    virtual void unlockClientMutex() noexcept =0;

    C4ExtraInfo extraInfo { };

protected:
    C4Database(std::string name, std::string dir, const C4DatabaseConfig&);
    static bool deleteDatabaseFileAtPath(const std::string &dbPath, C4StorageEngine);

    std::string const           _name;                  // Database filename (w/o extension)
    std::string const           _parentDirectory;
    C4DatabaseConfig2           _config;                // Configuration
    C4DatabaseConfig            _configV1;              // TODO: DEPRECATED
    mutable C4Collection* C4NULLABLE _defaultCollection = nullptr;
};

C4_ASSUME_NONNULL_END
