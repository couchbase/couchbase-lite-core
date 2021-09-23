//
// DataFile.hh
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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
#include "c4Compat.h"
#include "KeyStore.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "RefCounted.hh"
#include "InstanceCounted.hh"          // For fleece::InstanceCountedIn
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic> // for std::atomic_uint
#include <functional> // for std::function
#ifdef check
#undef check
#endif

namespace fleece { namespace impl {
    class Dict;
    class SharedKeys;
    class PersistentSharedKeys;
} }

typedef C4_ENUM(uint32_t, C4DatabaseTag) {
    DatabaseTagExternal,
    DatebaseTagReplicator,
    DatabaseTagBgDB,
    DatabaseTagREST,
    DatabaseTagOther
};

namespace litecore {

    class Query;
    class Transaction;
    class SequenceTracker;


    /** A database file, primarily a container of KeyStores which store the actual data.
        This is an abstract class, with concrete subclasses for different database engines. */
    class DataFile : public Logging, public fleece::InstanceCountedIn<DataFile> {
    public:

        class Delegate {
        public:
            virtual ~Delegate() =default;
            // Callback that takes a record body and returns the portion of it containing Fleece data
            virtual slice fleeceAccessor(slice recordBody) const =0;
            // Callback that takes a blob dictionary and returns the blob data
            virtual alloc_slice blobAccessor(const fleece::impl::Dict*) const =0;
            // Notifies that another DataFile on the same physical file has committed a transaction
            virtual void externalTransactionCommitted(const SequenceTracker &sourceTracker) { }
        };

        struct Options {
            KeyStore::Capabilities keyStores;
            bool                create         :1;      ///< Should the db be created if it doesn't exist?
            bool                writeable      :1;      ///< If false, db is opened read-only
            bool                useDocumentKeys:1;      ///< Use SharedKeys for Fleece docs
            bool                upgradeable    :1;      ///< DB schema can be upgraded
            EncryptionAlgorithm encryptionAlgorithm;    ///< What encryption (if any)
            alloc_slice         encryptionKey;          ///< Encryption key, if encrypting
            C4DatabaseTag       dbTag;
            static const Options defaults;
        };

        DataFile(const FilePath &path, Delegate* delegate NONNULL, const Options* =nullptr);
        virtual ~DataFile();

        FilePath filePath() const noexcept                  {return _path;}
        const Options& options() const noexcept             {return _options;}

        bool isClosing() const noexcept                     {return _closeSignaled;}
        virtual bool isOpen() const noexcept = 0;

        /** Throws an exception if the database is closed. */
        void checkOpen() const;

        /** Closes the database. Do not call any methods on this object afterwards,
            except isOpen() or mustBeOpen(), before deleting it. */
        void close(bool forDelete =false);

        /** Closes the database and deletes its file. */
        void deleteDataFile();

        /** Opens another instance on the same file. */
        DataFile* openAnother(Delegate* NONNULL);
        
        C4DatabaseTag databaseTag() const {
            return _options.dbTag;
        }
        
        void setDatabaseTag(C4DatabaseTag dbTag) {
            _options.dbTag = dbTag;
        }

        virtual uint64_t fileSize();

        /** Types of things \ref maintenance() can do.
            NOTE: If you update this, you must update C4MaintenanceType in c4Database.h too! */
        enum MaintenanceType {
            kCompact,
            kReindex,
            kIntegrityCheck,
        };

        /** Perform database maintenance of some type. Returns false if not supported. */
        virtual void maintenance(MaintenanceType) =0;

        virtual void rekey(EncryptionAlgorithm, slice newKey);

        Delegate* delegate() const                          {return _delegate;}
        fleece::impl::SharedKeys* documentKeys() const;


        void forOtherDataFiles(function_ref<void(DataFile*)> fn);

        /** Private API to run a raw (e.g. SQL) query, for diagnostic purposes only */
        virtual fleece::alloc_slice rawQuery(const std::string &query) =0;

        // to be called only by Query:
        void registerQuery(Query *query)        {_queries.insert(query);}
        void unregisterQuery(Query *query)      {_queries.erase(query);}

        //////// KEY-STORES:

        static const std::string kDefaultKeyStoreName;
        static const std::string kInfoKeyStoreName;

        /** The DataFile's default key-value store. */
        KeyStore& defaultKeyStore() const           {return defaultKeyStore(_options.keyStores);}
        KeyStore& defaultKeyStore(KeyStore::Capabilities) const;

        KeyStore& getKeyStore(const std::string &name) const;
        KeyStore& getKeyStore(const std::string &name, KeyStore::Capabilities) const;

#if 0 //UNUSED:
        /** The names of all existing KeyStores (whether opened yet or not) */
        virtual std::vector<std::string> allKeyStoreNames() =0;
#endif
        
        void closeKeyStore(const std::string &name);

#if ENABLE_DELETE_KEY_STORES
        /** Permanently deletes a KeyStore. */
        virtual void deleteKeyStore(const std::string &name) =0;
#endif

        // Redeclare logging methods as public, so Database can use them
        bool willLog(LogLevel level =LogLevel::Info) const         {return Logging::willLog(level);}
        void _logInfo(const char *format, ...) const __printflike(2, 3)   {LOGBODY(Info)}
        void _logVerbose(const char *format, ...) const __printflike(2, 3){LOGBODY(Verbose)}
        void _logDebug(const char *format, ...) const __printflike(2, 3)  {LOGBODY(Debug)}

        //////// SHARED OBJECTS:

        Retained<RefCounted> sharedObject(const std::string &key);
        Retained<RefCounted> addSharedObject(const std::string &key, Retained<RefCounted>);

        //////// FACTORY:

        /** Abstract factory for creating/managing DataFiles. */
        class Factory {
        public:
            std::string name()  {return std::string(cname());}
            virtual const char* cname() =0;
            virtual std::string filenameExtension() =0;
            virtual bool encryptionEnabled(EncryptionAlgorithm) =0;

            /** Opens a DataFile. */
            virtual DataFile* openFile(const FilePath &path,
                                       Delegate *delegate,
                                       const Options* =nullptr) =0;

            /** Deletes a non-open file. Returns false if it doesn't exist. */
            bool deleteFile(const FilePath &path, const Options* =nullptr);

            /** Moves a non-open file. */
            virtual void moveFile(const FilePath &fromPath, const FilePath &toPath);

            /** Does a file exist at this path? */
            virtual bool fileExists(const FilePath &path);
            
        protected:
            /** Deletes a non-open file. Returns false if it doesn't exist. */
            virtual bool _deleteFile(const FilePath &path, const Options* =nullptr) =0;

            virtual ~Factory() { }
            friend class DataFile;
        };

        static std::vector<Factory*> factories();
        static Factory* factoryNamed(const std::string &name);
        static Factory* factoryNamed(const char *name);
        static Factory* factoryForFile(const FilePath&);

    protected:
        virtual std::string loggingIdentifier() const override;

        /** Reopens database after it's been closed. */
        virtual void reopen();

        /** Override to close the actual database. (Called by close())*/
        virtual void _close(bool forDelete) =0;

        /** Override to instantiate a KeyStore object. */
        virtual KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) =0;

        /** Override to begin a database transaction. */
        virtual void _beginTransaction(Transaction* t NONNULL) =0;

        /** Override to commit or abort a database transaction. */
        virtual void _endTransaction(Transaction* t NONNULL, bool commit) =0;

        /** Is this DataFile object currently in a transaction? */
        bool inTransaction() const                      {return _inTransaction;}

        /** Override to begin a read-only transaction. */
        virtual void beginReadOnlyTransaction() =0;

        /** Override to end a read-only transaction. */
        virtual void endReadOnlyTransaction() =0;

        /** Runs the function/lambda while holding the file lock. This doesn't create a real
            transaction (at the ForestDB/SQLite/etc level), but it does ensure that no other thread
            is in a transaction, nor starts a transaction while the function is running. */
        void withFileLock(function_ref<void(void)> fn);

        void setOptions(const Options &o)               {_options = o;}

        void forOpenKeyStores(function_ref<void(KeyStore&)> fn);

        virtual Factory& factory() const =0;

    private:
        class Shared;
        friend class KeyStore;
        friend class Transaction;
        friend class ReadOnlyTransaction;
        friend class DocumentKeys;

        static bool deleteDataFile(DataFile *file, const Options *options,
                                   Shared *shared, Factory &factory);
        
        KeyStore& addKeyStore(const std::string &name, KeyStore::Capabilities);
        void beginTransactionScope(Transaction*);
        void transactionBegan(Transaction*);
        void transactionEnding(Transaction*, bool committing);
        void endTransactionScope(Transaction*);
        Transaction& transaction();

        DataFile(const DataFile&) = delete;
        DataFile& operator=(const DataFile&) = delete;

        Delegate* const         _delegate;
        Retained<Shared>        _shared;                        // Shared state of file (lock)
        FilePath const          _path;                          // Path as given (non-canonical)
        Options                 _options;                       // Option/capability flags
        mutable KeyStore*       _defaultKeyStore {nullptr};     // The default KeyStore
        std::unordered_map<std::string, std::unique_ptr<KeyStore>> _keyStores;// Opened KeyStores
        mutable Retained<fleece::impl::PersistentSharedKeys> _documentKeys;
        std::unordered_set<Query*> _queries;                    // Query objects
        bool                    _inTransaction {false};         // Am I in a Transaction?
        std::atomic_bool        _closeSignaled {false};         // Have I been asked to close?
    };


    /** Grants exclusive write access to a DataFile while in scope.
        The transaction is committed when the object exits scope, unless abort() was called.
        Only one Transaction object can be created on a database file at a time.
        Not just per DataFile object; per database _file_. */
    class Transaction {
    public:
        explicit Transaction(DataFile*);
        explicit Transaction(DataFile &db)  :Transaction(&db) { }
        explicit Transaction(const std::unique_ptr<DataFile>& db)  :Transaction(db.get()) { }
        ~Transaction();

        DataFile& dataFile() const          {return _db;}

        void commit();
        void abort();

        void notifyCommitted(SequenceTracker&);

    private:
        friend class DataFile;
        friend class KeyStore;

        Transaction(DataFile*, bool begin);
        Transaction(const Transaction&) = delete;

        DataFile&   _db;        // The DataFile
        bool _active;           // Is there an open transaction at the db level?
    };


    /** A read-only transaction. Does not grant access to writes, but ensures that all database
        reads are consistent with each other.
        Multiple DataFile instances on the same file may have simultaneous ReadOnlyTransactions,
        and they can coexist with a simultaneous Transaction (but will be isolated from its
        changes.) */
    class ReadOnlyTransaction {
    public:
        explicit ReadOnlyTransaction(DataFile *db);
        explicit ReadOnlyTransaction(DataFile &db)  :ReadOnlyTransaction(&db) { }
        ~ReadOnlyTransaction();
    private:
        ReadOnlyTransaction(const ReadOnlyTransaction&) = delete;

        DataFile *_db {nullptr};
    };

}
