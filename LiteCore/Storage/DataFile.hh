//
// DataFile.hh
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "KeyStore.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "fleece/InstanceCounted.hh"  // For fleece::InstanceCountedIn
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic>  // for std::atomic_uint
#ifdef check
#    undef check
#endif

namespace fleece::impl {
    class Dict;
    class SharedKeys;
    class PersistentSharedKeys;
}  // namespace fleece::impl

namespace litecore {

    class Query;
    class ExclusiveTransaction;
    class SequenceTracker;

    // Must match ::C4DatabaseTag, declared in c4Private.h
    enum DatabaseTag : uint32_t {
        kDatabaseTag_AppOpened,
        kDatabaseTag_DBAccess,
        kDatabaseTag_C4RemoteReplicator,
        kDatabaseTag_C4IncomingReplicator,
        kDatabaseTag_C4LocalReplicator1,
        kDatabaseTag_C4LocalReplicator2,
        kDatabaseTag_BackgroundDB,
        kDatabaseTag_RESTListener
    };

    /** A database file, primarily a container of KeyStores which store the actual data.
        This is an abstract class, with concrete subclasses for different database engines. */
    class DataFile
        : public Logging
        , public fleece::InstanceCountedIn<DataFile> {
      public:
        class Delegate {
          public:
            virtual ~Delegate() = default;
            // The user-visible name of this database
            [[nodiscard]] virtual string databaseName() const = 0;
            // Callback that takes a blob dictionary and returns the blob data
            virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const = 0;

            // Notifies that another DataFile on the same physical file has committed a transaction
            virtual void externalTransactionCommitted(const SequenceTracker& sourceTracker) {}

            // Notifies that another DataFile on the same physical file has deleted a collection
            virtual void collectionRemoved(const std::string& keyStoreName){};
        };

        struct Options {
            KeyStore::Capabilities keyStores;
            bool                   create : 1;           ///< Should the db be created if it doesn't exist?
            bool                   writeable : 1;        ///< If false, db is opened read-only
            bool                   useDocumentKeys : 1;  ///< Use SharedKeys for Fleece docs
            bool                   upgradeable : 1;      ///< DB schema can be upgraded
            bool                   diskSyncFull : 1;     ///< SQLite PRAGMA synchronous
            bool                   noHousekeeping : 1;   ///< Disable automatic maintenance
            EncryptionAlgorithm    encryptionAlgorithm;  ///< What encryption (if any)
            alloc_slice            encryptionKey;        ///< Encryption key, if encrypting
            DatabaseTag            dbTag;
            static const Options   defaults;
        };

        DataFile(const FilePath& path, Delegate* delegate NONNULL, const Options* = nullptr);
        ~DataFile() override;

        FilePath filePath() const noexcept { return _path; }

        const Options& options() const noexcept { return _options; }

        bool isClosing() const noexcept { return _closeSignaled; }

        virtual bool isOpen() const noexcept = 0;

        /** Throws an exception if the database is closed. */
        void checkOpen() const;

        /** Closes the database. Do not call any methods on this object afterwards,
            except isOpen() or mustBeOpen(), before deleting it. */
        void close(bool forDelete = false);

        /** Closes the database and deletes its file. */
        void deleteDataFile();

        /** Opens another instance on the same file. */
        DataFile* openAnother(Delegate* NONNULL);

        DatabaseTag databaseTag() const { return _options.dbTag; }

        void setDatabaseTag(DatabaseTag dbTag) { _options.dbTag = dbTag; }

        virtual uint64_t fileSize();

        /** Types of things \ref maintenance() can do.
            NOTE: If you update this, you must update C4MaintenanceType in c4DatabaseTypes.h too! */
        enum MaintenanceType {
            kCompact,
            kReindex,
            kIntegrityCheck,
            kQuickOptimize,  ///< Quickly update db statistics to help optimize queries
            kFullOptimize,   ///< Full update of db statistics; takes longer
        };

        /** Perform database maintenance of some type. Returns false if not supported. */
        virtual void maintenance(MaintenanceType) = 0;

        virtual void rekey(EncryptionAlgorithm, slice newKey);

        Delegate* delegate() const { return _delegate; }

        fleece::impl::SharedKeys* documentKeys() const;


        void forOtherDataFiles(function_ref<void(DataFile*)> fn);

        //////// QUERIES:

        /** Creates a database query object. */
        virtual Ref<Query> compileQuery(slice     expr, QueryLanguage = QueryLanguage::kJSON,
                                        KeyStore* defaultKeyStore = nullptr) = 0;

        /** Private API to run a raw (e.g. SQL) query, for diagnostic purposes only */
        virtual fleece::alloc_slice rawQuery(const std::string& query) = 0;

        /**
         * Private API to run a raw SQL query.
         * Intended for queries which return a single value (i.e. PRAGMA).
         * Returns a single value encoded into a slice, for convenience.
         *
         * Strings and blobs are returned as-is. Null is returned as nullslice. Numbers are encoded as strings.
         */
        virtual alloc_slice rawScalarQuery(const std::string& query) = 0;

        // to be called only by Query:
        void registerQuery(Query* query);
        void unregisterQuery(Query* query);

        //////// KEY-STORES:

        static const std::string kDefaultKeyStoreName;
        static const std::string kInfoKeyStoreName;

        /** The DataFile's default key-value store. */
        KeyStore& defaultKeyStore() const { return defaultKeyStore(_options.keyStores); }

        KeyStore& defaultKeyStore(KeyStore::Capabilities) const;

        KeyStore& getKeyStore(slice name) const;
        KeyStore& getKeyStore(slice name, KeyStore::Capabilities) const;

        virtual bool keyStoreExists(const std::string& name) const = 0;

        /** The names of all existing KeyStores (whether opened yet or not) */
        virtual std::vector<std::string> allKeyStoreNames() const = 0;

        void closeKeyStore(const std::string& name);

        /** Permanently deletes a KeyStore. */
        virtual void deleteKeyStore(const std::string& name) = 0;

        // Redeclare logging methods as public, so Database can use them
        bool willLog(LogLevel level = LogLevel::Info) const { return Logging::willLog(level); }

        void _logWarning(const char* format, ...) const __printflike(2, 3) { LOGBODY(Warning) }

        void _logInfo(const char* format, ...) const __printflike(2, 3) { LOGBODY(Info) }

        void _logVerbose(const char* format, ...) const __printflike(2, 3) { LOGBODY(Verbose) }

        void _logDebug(const char* format, ...) const __printflike(2, 3) { LOGBODY(Debug) }

        void _log(LogLevel level, const char* format, ...) const __printflike(3, 4){LOGBODY_(level)}

        //////// SHARED OBJECTS:

        Retained<RefCounted> sharedObject(const std::string& key);
        Ref<RefCounted> addSharedObject(const std::string& key, RefCounted*);

        //////// FACTORY:

        /** Abstract factory for creating/managing DataFiles. */
        class Factory {
          public:
            std::string name() { return {cname()}; }

            virtual const char* cname()                                = 0;
            virtual std::string filenameExtension()                    = 0;
            virtual bool        encryptionEnabled(EncryptionAlgorithm) = 0;

            /** Opens a DataFile. */
            virtual DataFile* openFile(const FilePath& path, Delegate* delegate, const Options* = nullptr) = 0;

            /** Deletes a non-open file. Returns false if it doesn't exist. */
            bool deleteFile(const FilePath& path, const Options* = nullptr);

            /** Moves a non-open file. */
            virtual void moveFile(const FilePath& fromPath, const FilePath& toPath);

            /** Does a file exist at this path? */
            virtual bool fileExists(const FilePath& path);

          protected:
            /** Deletes a non-open file. Returns false if it doesn't exist. */
            virtual bool _deleteFile(const FilePath& path, const Options* = nullptr) = 0;

            virtual ~Factory() = default;
            friend class DataFile;
        };

        static std::vector<Factory*> factories();
        static Factory*              factoryNamed(const std::string& name);
        static Factory*              factoryNamed(const char* name);
        static Factory*              factoryForFile(const FilePath&);

        static bool isDefaultCollection(slice id) { return id == KeyStore::kDefaultCollectionName; }

        static bool isDefaultScope(slice id) { return !id || isDefaultCollection(id); }

        // kScopeCollectionSeparator must not be escaped as it separates the scope from the
        // generalized collection name, a.k.a. collection path.
        // This function returns the position of unescaped separator starting from pos.
        // It returns string::npos if not found.
        static size_t findCollectionPathSeparator(string_view connectionPath, size_t pos = 0);
        // After separating out the scope from collection path by kScopeCollectionSeparator ('.'),
        // the following function can be used to unescape the escaped separator.
        static string unescapeCollectionName(const string& unescaped);

        static std::pair<alloc_slice, alloc_slice> splitCollectionPath(const string& collectionPath);

        DataFile(const DataFile&)            = delete;
        DataFile& operator=(const DataFile&) = delete;

      protected:
        std::string loggingIdentifier() const override;

        /** Reopens database after it's been closed. */
        virtual void reopen();

        /** Override to close the actual database. (Called by close())*/
        virtual void _close(bool forDelete) = 0;

        /** Override to instantiate a KeyStore object. */
        virtual KeyStore* newKeyStore(const std::string& name, KeyStore::Capabilities) = 0;

        /** Override to begin a database transaction. */
        virtual void _beginTransaction(ExclusiveTransaction* t NONNULL) = 0;

        /** Override to commit or abort a database transaction. */
        virtual void _endTransaction(ExclusiveTransaction* t NONNULL, bool commit) = 0;

        /** Is this DataFile object currently in a transaction? */
        bool inTransaction() const { return _inTransaction; }

        /** Override to begin a read-only transaction. */
        virtual void beginReadOnlyTransaction() = 0;

        /** Override to end a read-only transaction. */
        virtual void endReadOnlyTransaction() = 0;

        /** Runs the function/lambda while holding the file lock. This doesn't create a real
            transaction (at the ForestDB/SQLite/etc level), but it does ensure that no other thread
            is in a transaction, nor starts a transaction while the function is running. */
        void withFileLock(function_ref<void(void)> fn);

        void setOptions(const Options& o) { _options = o; }

        const Options& getOptions() const { return _options; }

        void forOpenKeyStores(function_ref<void(KeyStore&)> fn);

        virtual Factory& factory() const = 0;

      private:
        class Shared;
        friend class KeyStore;
        friend class ExclusiveTransaction;
        friend class ReadOnlyTransaction;
        friend class DocumentKeys;

        static bool deleteDataFile(DataFile* file, const Options* options, Shared* shared, Factory& factory);

        KeyStore&             addKeyStore(const std::string& name, KeyStore::Capabilities);
        void                  closeAllQueries();
        void                  beginTransactionScope(ExclusiveTransaction*);
        void                  transactionBegan(ExclusiveTransaction*);
        void                  transactionEnding(ExclusiveTransaction*, bool committing);
        void                  endTransactionScope(ExclusiveTransaction*);
        ExclusiveTransaction& transaction();

        Delegate* const                                       _delegate;
        Retained<Shared>                                      _shared;   // Shared state of file (lock)
        FilePath const                                        _path;     // Path as given (non-canonical)
        Options                                               _options;  // Option/capability flags
        mutable KeyStore*                                     _defaultKeyStore{nullptr};  // The default KeyStore
        std::unordered_map<std::string, unique_ptr<KeyStore>> _keyStores;                 // Opened KeyStores
        mutable Retained<fleece::impl::PersistentSharedKeys>  _documentKeys;
        std::unordered_set<Query*>                            _queries;               // Query objects
        std::mutex                                            _queriesMutex;          // Thread-safe access to _queries
        bool                                                  _inTransaction{false};  // Am I in a Transaction?
        std::atomic_bool                                      _closeSignaled{false};  // Have I been asked to close?
        mutable std::once_flag                                _documentKeysOnce{};  // Thread-safe init of documentKeys
    };

    /** Grants exclusive write access to a DataFile while in scope.
        The transaction is committed when the object exits scope, unless abort() was called.
        Only one Transaction object can be created on a database file at a time.
        Not just per DataFile object; per database _file_.
        That means THESE DO NOT NEST! (The higher level C4Database::Transaction does nest.) */
    class ExclusiveTransaction {
      public:
        explicit ExclusiveTransaction(DataFile*);

        explicit ExclusiveTransaction(DataFile& db) : ExclusiveTransaction(&db) {}

        explicit ExclusiveTransaction(const unique_ptr<DataFile>& db) : ExclusiveTransaction(db.get()) {}

        ~ExclusiveTransaction();

        [[nodiscard]] DataFile& dataFile() const { return _db; }

        void commit();
        void abort();

        void notifyCommitted(SequenceTracker&);

        ExclusiveTransaction(const ExclusiveTransaction&) = delete;

      private:
        friend class DataFile;
        friend class KeyStore;

        ExclusiveTransaction(DataFile*, bool begin);

        DataFile& _db;      // The DataFile
        bool      _active;  // Is there an open transaction at the db level?
    };

    /** A read-only transaction. Does not grant access to writes, but ensures that all database
        reads are consistent with each other.
        Multiple DataFile instances on the same file may have simultaneous ReadOnlyTransactions,
        and they can coexist with a simultaneous Transaction (but will be isolated from its
        changes.) */
    class ReadOnlyTransaction {
      public:
        explicit ReadOnlyTransaction(DataFile* db);

        explicit ReadOnlyTransaction(DataFile& db) : ReadOnlyTransaction(&db) {}

        ~ReadOnlyTransaction();

        ReadOnlyTransaction(const ReadOnlyTransaction&) = delete;

      private:
        DataFile* _db{nullptr};
    };

}  // namespace litecore
