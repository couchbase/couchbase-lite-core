//
//  DataFile.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#pragma once
#include "KeyStore.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include <vector>
#include <unordered_map>
#include <atomic> // for std::atomic_uint
#ifdef check
#undef check
#endif

namespace fleece {
    class SharedKeys;
    class PersistentSharedKeys;
}

namespace litecore {

    class Transaction;

    extern LogDomain DBLog;


    /** A database file, primarily a container of KeyStores which store the actual data.
        This is an abstract class, with concrete subclasses for different database engines. */
    class DataFile {
    public:

        struct Options {
            KeyStore::Capabilities keyStores;
            bool create         :1;     ///< Should the db be created if it doesn't exist?
            bool writeable      :1;     ///< If false, db is opened read-only
            EncryptionAlgorithm encryptionAlgorithm;
            alloc_slice encryptionKey;

            static const Options defaults;
        };

        DataFile(const FilePath &path, const Options* =nullptr);
        virtual ~DataFile();

        const FilePath& filePath() const noexcept;
        const Options& options() const noexcept              {return _options;}

        virtual bool isOpen() const noexcept =0;

        /** Throws an exception if the database is closed. */
        void checkOpen() const;

        /** Closes the database. Do not call any methods on this object afterwards,
            except isOpen() or mustBeOpen(), before deleting it. */
        virtual void close();

        /** Reopens database after it's been closed. */
        virtual void reopen() =0;

        /** Closes the database and deletes its file. */
        virtual void deleteDataFile() =0;

        virtual void compact() =0;
        bool isCompacting() const noexcept;
        static bool isAnyCompacting() noexcept;

        typedef std::function<void(bool compacting)> OnCompactCallback;

        void setOnCompact(OnCompactCallback callback) noexcept  {_onCompactCallback = callback;}

        virtual bool setAutoCompact(bool autoCompact)   {return false;}

        virtual void rekey(EncryptionAlgorithm, slice newKey);

        /** The number of soft deletions that have been purged via compaction. 
            (Used by the indexer) */
        uint64_t purgeCount() const;

        void useDocumentKeys();
        fleece::SharedKeys* documentKeys() const          {return (fleece::SharedKeys*)_documentKeys.get();}

        //////// KEY-STORES:

        static const std::string kDefaultKeyStoreName;
        static const std::string kInfoKeyStoreName;

        /** The DataFile's default key-value store. */
        KeyStore& defaultKeyStore() const           {return defaultKeyStore(_options.keyStores);}
        KeyStore& defaultKeyStore(KeyStore::Capabilities) const;

        KeyStore& getKeyStore(const std::string &name) const;
        KeyStore& getKeyStore(const std::string &name, KeyStore::Capabilities) const;

        /** The names of all existing KeyStores (whether opened yet or not) */
        virtual std::vector<std::string> allKeyStoreNames() =0;

        void closeKeyStore(const std::string &name);

        /** Permanently deletes a KeyStore. */
        virtual void deleteKeyStore(const std::string &name) =0;


        /** Abstract factory for creating/managing DataFiles. */
        class Factory {
        public:
            std::string name()  {return std::string(cname());}
            virtual const char* cname() =0;
            virtual std::string filenameExtension() =0;
            virtual bool encryptionEnabled(EncryptionAlgorithm) =0;

            /** Opens a DataFile. */
            virtual DataFile* openFile(const FilePath &path, const Options* =nullptr) =0;

            /** Deletes a non-open file. */
            virtual bool deleteFile(const FilePath &path, const Options* =nullptr);

            /** Moves a non-open file. */
            virtual void moveFile(const FilePath &fromPath, const FilePath &toPath);

            /** Does a file exist at this path? */
            virtual bool fileExists(const FilePath &path);
            
        protected:
            virtual ~Factory() { }
        };

        static std::vector<Factory*> factories();
        static Factory* factoryNamed(const std::string &name);
        static Factory* factoryNamed(const char *name);
        static Factory* factoryForFile(const FilePath&);

    protected:
        /** Override to instantiate a KeyStore object. */
        virtual KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) =0;

        /** Override to begin a database transaction. */
        virtual void _beginTransaction(Transaction*) =0;

        /** Override to commit or abort a database transaction. */
        virtual void _endTransaction(Transaction*, bool commit) =0;

        /** Is this DataFile object currently in a transaction? */
        bool inTransaction() const                      {return _inTransaction;}

        /** Runs the function/lambda while holding the file lock. This doesn't create a real
            transaction (at the ForestDB/SQLite/etc level), but it does ensure that no other thread
            is in a transaction, nor starts a transaction while the function is running. */
        void withFileLock(std::function<void(void)> fn);

        void updatePurgeCount(Transaction&);

        void beganCompacting();
        void finishedCompacting();

        void setOptions(const Options &o)               {_options = o;}

        void forOpenKeyStores(std::function<void(KeyStore&)> fn);

    private:
        class File;
        friend class KeyStore;
        friend class Transaction;
        friend class DocumentKeys;

        KeyStore& addKeyStore(const std::string &name, KeyStore::Capabilities);
        void beginTransactionScope(Transaction*);
        void transactionBegan(Transaction*);
        void transactionEnding(Transaction*, bool committing);
        void endTransactionScope(Transaction*);
        Transaction& transaction();

        DataFile(const DataFile&) = delete;
        DataFile& operator=(const DataFile&) = delete;

        void incrementDeletionCount(Transaction &t);

        File* const             _file;                          // Shared state of file (lock)
        Options                 _options;                       // Option/capability flags
        KeyStore*               _defaultKeyStore {nullptr};     // The default KeyStore
        std::unordered_map<std::string, std::unique_ptr<KeyStore>> _keyStores;// Opened KeyStores
        OnCompactCallback       _onCompactCallback {nullptr};   // Client callback for compacts
        std::unique_ptr<fleece::PersistentSharedKeys> _documentKeys;
        bool _inTransaction     {false};                        // Am I in a Transaction?
    };


    /** Grants exclusive write access to a DataFile while in scope.
        The transaction is committed when the object exits scope, unless abort() was called.
        Only one Transaction object can be created on a database file at a time.
        Not just per DataFile object; per database _file_. */
    class Transaction {
    public:
        explicit Transaction(DataFile*);
        Transaction(DataFile &db)               :Transaction(&db) { }
        ~Transaction();

        DataFile& dataFile() const          {return _db;}

        void commit();
        void abort();

    private:
        friend class DataFile;
        friend class KeyStore;

        void incrementDeletionCount()       {_db.incrementDeletionCount(*this);}

        Transaction(DataFile*, bool begin);
        Transaction(const Transaction&) = delete;

        DataFile&   _db;        // The DataFile
        bool _active;           // Is there an open transaction at the db level?
    };

}
