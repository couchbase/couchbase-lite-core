//
// c4Database.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4DatabaseTypes.h"
#include "c4DocumentTypes.h"
#include "c4IndexTypes.h"
#include "c4QueryTypes.h"
#include "fleece/function_ref.hh"
#include "fleece/InstanceCounted.hh"
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
C4EncryptionKey C4EncryptionKeyFromPassword(fleece::slice password, C4EncryptionAlgorithm = kC4EncryptionAES256);

/// Derives an encryption key from a user-entered password with SHA1 as the hashing function
C4EncryptionKey C4EncryptionKeyFromPasswordSHA1(fleece::slice password, C4EncryptionAlgorithm = kC4EncryptionAES256);

/// A LiteCore database connection.
struct C4Database
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4Database>
    , C4Base {
  public:
    // Lifecycle:

    using Config = C4DatabaseConfig2;

    /** Registers a directory path to load extension libraries from, such as Vector Search.
        Must be called before opening a database that will use an extension. */
    static void setExtensionPath(slice path);

    static bool exists(slice name, slice inDirectory);
    static void copyNamed(slice sourcePath, slice destinationName, const Config&);
    static bool deleteNamed(slice name, slice inDirectory);
    static bool deleteAtPath(slice path);

    static Retained<C4Database> openNamed(slice name, const Config&);

    static Retained<C4Database> openAtPath(slice path, C4DatabaseFlags, const C4EncryptionKey* C4NULLABLE = nullptr);

    static void shutdownLiteCore();

    Retained<C4Database> openAgain() const { return openNamed(getName(), getConfiguration()); }

    virtual void close()                                      = 0;
    virtual void closeAndDeleteFile()                         = 0;
    virtual void rekey(const C4EncryptionKey* C4NULLABLE key) = 0;
    virtual void maintenance(C4MaintenanceType)               = 0;

    // Attributes:

    slice getName() const noexcept FLPURE { return _name; }

    virtual alloc_slice getPath() const = 0;

    const Config& getConfiguration() const noexcept FLPURE { return _config; }

    virtual alloc_slice getSourceID() const    = 0;
    virtual C4UUID      getPublicUUID() const  = 0;
    virtual C4UUID      getPrivateUUID() const = 0;

    // Scopes:

    using ScopeCallback = fleece::function_ref<void(slice)>;

    /// Calls the callback function for each scope ID, in the same order as getScopeIDs().
    virtual void forEachScope(const ScopeCallback&) const = 0;

    // Collections:

    /// Easier version of `C4CollectionSpec`.
    /// You can pass a `CollectionSpec` parameter as simply a collection name slice, implying the
    /// default scope; or as `{collectionname, scopename}`.
    struct CollectionSpec : public C4CollectionSpec {
        CollectionSpec() : C4CollectionSpec{kC4DefaultCollectionName, kC4DefaultScopeID} {}

        // The single-arg constructors must be implicit, otherwise half the code breaks
        // NOLINTBEGIN(google-explicit-constructor)
        CollectionSpec(const C4CollectionSpec& spec) : C4CollectionSpec(spec) {}

        CollectionSpec(FLString name, FLString scope) : C4CollectionSpec{name, scope} {}

        CollectionSpec(FLString name) : C4CollectionSpec{name, kC4DefaultScopeID} {}

        CollectionSpec(slice name, slice scope) : C4CollectionSpec{name, scope} {}

        CollectionSpec(slice name) : C4CollectionSpec{name, kC4DefaultScopeID} {}

        // NOLINTEND(google-explicit-constructor)
    };

    /// Returns the default collection, whose name is "_default" (`kC4DefaultCollectionName`).
    C4Collection* C4NULLABLE getDefaultCollection() const;

    /// Returns true if a collection exists with the given name & scope.
    virtual bool hasCollection(CollectionSpec) const = 0;

    /// Returns true if a scope exists with the given name
    /// (in other words, if there are any collections in the scope)
    /// _default will always return true
    virtual bool hasScope(C4String) const = 0;

    /// Returns the existing collection with the given name & scope, or nullptr if it doesn't exist.
    virtual C4Collection* C4NULLABLE getCollection(CollectionSpec) const = 0;

    /// Creates and returns an empty collection with the given name in the given scope,
    /// or if one already exists, returns that.
    virtual C4Collection* createCollection(CollectionSpec) = 0;

    /// Deletes the collection with the given name & scope.
    virtual void deleteCollection(CollectionSpec) = 0;

    using CollectionSpecCallback = fleece::function_ref<void(CollectionSpec)>;

    /// Calls the callback function for each collection in the scope, in the order created.
    void forEachCollection(slice scopeName, const CollectionSpecCallback&) const;

    /// Calls the callback function for each collection _in each scope_.
    virtual void forEachCollection(const CollectionSpecCallback&) const = 0;

#ifndef C4_STRICT_COLLECTION_API
    // Shims to ease the pain of converting to collections. These delegate to the default collection.
    uint64_t             getDocumentCount() const;
    C4SequenceNumber     getLastSequence() const;
    Retained<C4Document> getDocument(slice docID, bool mustExist = true,
                                     C4DocContentLevel content = kDocGetCurrentRev) const;
    Retained<C4Document> getDocumentBySequence(C4SequenceNumber sequence) const;
    Retained<C4Document> putDocument(const C4DocPutRequest& rq, size_t* C4NULLABLE outCommonAncestorIndex,
                                     C4Error* outError);
    bool                 purgeDocument(slice docID);
    C4Timestamp          getExpiration(slice docID) const;
    bool                 setExpiration(slice docID, C4Timestamp timestamp);
#endif

    // Transactions:

    /** Manages a transaction safely. The constructor begins a transaction, and \ref commit
        commits it. If the Transaction object exits scope without being committed, it aborts. */
    class Transaction {
      public:
        explicit Transaction(C4Database* db) : _db(db) { db->beginTransaction(); }

        Transaction(Transaction&& t) noexcept : _db(t._db) { t._db = nullptr; }

        void commit() {
            auto db = _db;
            _db     = nullptr;
            db->endTransaction(true);
        }

        void abort() {
            auto db = _db;
            _db     = nullptr;
            db->endTransaction(false);
        }

        ~Transaction() {
            if ( _db ) _db->endTransaction(false);
        }

        Transaction(const Transaction&)            = delete;
        Transaction& operator=(const Transaction&) = delete;

      private:
        C4Database* C4NULLABLE _db;
    };

    virtual bool isInTransaction() const noexcept FLPURE = 0;

    // Raw Documents:

    static constexpr slice kInfoStore = "info";  /// Raw-document store used for db metadata.

    virtual bool getRawDocument(slice storeName, slice key,
                                fleece::function_ref<void(C4RawDocument* C4NULLABLE)> callback) = 0;

    virtual void putRawDocument(slice storeName, const C4RawDocument&) = 0;

    // Fleece-related utilities for document encoding:

    virtual alloc_slice  encodeJSON(slice jsonData) const = 0;
    virtual FLEncoder    createFleeceEncoder() const      = 0;
    virtual FLEncoder    sharedFleeceEncoder() const      = 0;
    virtual FLSharedKeys getFleeceSharedKeys() const      = 0;

    // Expiration:

    virtual C4Timestamp nextDocExpiration() const = 0;

    // Blobs:

    virtual C4BlobStore& getBlobStore() const = 0;

    // Queries & Indexes:

    Retained<C4Query> newQuery(C4QueryLanguage language, slice queryExpression,
                               int* C4NULLABLE outErrorPos = nullptr) const;

#ifndef C4_STRICT_COLLECTION_API
    void        createIndex(slice name, slice indexSpec, C4QueryLanguage indexSpecLanguage, C4IndexType indexType,
                            const C4IndexOptions* C4NULLABLE indexOptions = nullptr);
    void        deleteIndex(slice name);
    alloc_slice getIndexesInfo(bool fullInfo = true) const;
    alloc_slice getIndexRows(slice name) const;
#endif

    // Replicator:

    Retained<C4Replicator> newReplicator(C4Address serverAddress, slice remoteDatabaseName,
                                         const C4ReplicatorParameters& params);

    Retained<C4Replicator> newIncomingReplicator(C4Socket* openSocket, const C4ReplicatorParameters& params);
    Retained<C4Replicator> newIncomingReplicator(litecore::websocket::WebSocket* openSocket,
                                                 const C4ReplicatorParameters&   params);

#ifdef COUCHBASE_ENTERPRISE
    Retained<C4Replicator> newLocalReplicator(C4Database* otherLocalDB, const C4ReplicatorParameters& params);
#endif

    alloc_slice getCookies(const C4Address&);

    bool setCookie(slice setCookieHeader, slice fromHost, slice fromPath, bool acceptParentDomain);

    void clearCookies();

    // only used internally:
    // These are used by the replicator:
    virtual C4RemoteID  getRemoteDBID(slice remoteAddress, bool canCreate) = 0;
    virtual alloc_slice getRemoteDBAddress(C4RemoteID remoteID)            = 0;
    virtual alloc_slice getRevIDGlobalForm(slice revID)                    = 0;

    // Used only by the `cblite` tool:
    virtual alloc_slice rawQuery(slice sqliteQuery) = 0;

    // Only for use by the C API -- internal or deprecated:
    virtual void beginTransaction()          = 0;  // use Transaction class above instead
    virtual void endTransaction(bool commit) = 0;
    static void  copyFileToPath(slice sourcePath, slice destinationPath, const C4DatabaseConfig&);

    const C4DatabaseConfig& configV1() const noexcept FLPURE { return _configV1; }

    virtual void lockClientMutex() noexcept   = 0;
    virtual void unlockClientMutex() noexcept = 0;

    C4ExtraInfo extraInfo{};

  protected:
    C4Database(std::string name, std::string dir, const C4DatabaseConfig&);
    static bool   deleteDatabaseFileAtPath(const std::string& dbPath, C4StorageEngine);
    C4Collection* getDefaultCollectionSafe() const;  // Same as getDefaultCollection except throws an error when null
    virtual void  checkOpen() const = 0;

    std::string const                _name;  // Database filename (w/o extension)
    std::string const                _parentDirectory;
    C4DatabaseConfig2                _config;    // Configuration
    C4DatabaseConfig                 _configV1;  // TODO: DEPRECATED
    mutable C4Collection* C4NULLABLE _defaultCollection = nullptr;
};

// This stuff allows CollectionSpec to be used as a key in an unordered_map or unordered_set:
static inline bool operator==(const C4CollectionSpec& a, const C4CollectionSpec& b) {
    return a.name == b.name && a.scope == b.scope;
}

static inline bool operator!=(const C4CollectionSpec& a, const C4CollectionSpec& b) { return !(a == b); }

template <>
struct std::hash<C4CollectionSpec> {
    std::size_t operator()(C4CollectionSpec const& spec) const {
        return fleece::slice(spec.name).hash() ^ fleece::slice(spec.scope).hash();
    }
};

template <>
struct std::hash<C4Database::CollectionSpec> : public std::hash<C4CollectionSpec> {};

C4_ASSUME_NONNULL_END
