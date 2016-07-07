//
//  Database.hh
//  CBForest
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__Database__
#define __CBForest__Database__
#include "KeyStore.hh"
#include <vector>
#include <unordered_map>
#include <atomic> // for std::atomic_uint
#ifdef check
#undef check
#endif

namespace cbforest {

    class Transaction;


    enum logLevel {
        kDebug,
        kInfo,
        kWarning,
        kError,
        kNone
    };
    extern logLevel LogLevel;
    extern void (*LogCallback)(logLevel, const char *message);


    /** ForestDB database; primarily a container of KeyStores.
        A Database also acts as its default KeyStore. */
    class Database : public KeyStore {
    public:

        typedef ::fdb_file_info info;
        typedef ::fdb_config config;

        static config defaultConfig();
        static void setDefaultConfig(const config&);

        Database(std::string path, const config&);
        Database(Database* original, sequence snapshotSequence);
        virtual ~Database();

        const std::string& filename() const;
        info getInfo() const;
        config getConfig()                      {return _config;}

        /** The number of deletions that have been purged via compaction. (Used by the indexer) */
        uint64_t purgeCount() const;

        bool isReadOnly() const;

        bool isOpen()                           {return _fileHandle != NULL;}

        /** Closes the database. Do not call any methods on this object afterwards,
            except isOpen() or mustBeOpen(), before deleting it. */
        void close();

        /** Closes the database and deletes its file. */
        void deleteDatabase();

        /** Reopens database after it's been closed. */
        void reopen();

        /** Deletes a database that isn't open. */
        static void deleteDatabase(std::string path, const config&);

        void compact();
        bool isCompacting() const               {return _isCompacting;}
        static bool isAnyCompacting();
        void setCompactionMode(fdb_compaction_mode_t);

        typedef void (*OnCompactCallback)(void *context, bool compacting);

        void setOnCompact(OnCompactCallback callback, void *context) {
            _onCompactCallback = callback;
            _onCompactContext = context;
        }

        void rekey(const fdb_encryption_key&);

        /** The Database's default key-value store. (You can also just use the Database
            instance directly as a KeyStore since it inherits from it.) */
        const KeyStore& defaultKeyStore() const {return *this;}
        KeyStore& defaultKeyStore()             {return *this;}

        KeyStore& getKeyStore(std::string name) const;

        bool contains(KeyStore&) const;

        void closeKeyStore(std::string name);
        void deleteKeyStore(std::string name);

    private:
        class File;
        friend class KeyStore;
        friend class Transaction;
        fdb_kvs_handle* openKVS(std::string name) const;
        void beginTransaction(Transaction*);
        void endTransaction(Transaction*);

        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        void incrementDeletionCount(Transaction *t);
        void updatePurgeCount();
        static fdb_compact_decision compactionCallback(fdb_file_handle *fhandle,
                                                       fdb_compaction_status status,
                                                       const char *kv_store_name,
                                                       fdb_doc *doc,
                                                       uint64_t last_oldfile_offset,
                                                       uint64_t last_newfile_offset,
                                                       void *ctx);
        bool onCompact(fdb_compaction_status status,
                       const char *kv_store_name,
                       fdb_doc *doc,
                       uint64_t lastOldFileOffset,
                       uint64_t lastNewFileOffset);

        File* _file;
        config _config;
        fdb_file_handle* _fileHandle {nullptr};
        std::unordered_map<std::string, std::unique_ptr<KeyStore> > _keyStores;
        bool _inTransaction {false};
        bool _isCompacting {false};
        OnCompactCallback _onCompactCallback {nullptr};
        void  *_onCompactContext {nullptr};
    };


    /** Grants exclusive write access to a Database while in scope.
        The transaction is committed when the object exits scope, unless abort() was called.
        Only one Transaction object can be created on a database file at a time.
        Not just per Database object; per database _file_. */
    class Transaction : public KeyStoreWriter {
    public:
        enum state {
            kNoOp,
            kAbort,
            kCommit,
            kCommitManualWALFlush
        };

        Transaction(Database*);
        ~Transaction()                          {_db.endTransaction(this);}

        /** Converts a KeyStore to a KeyStoreWriter to allow write access. */
        KeyStoreWriter operator() (KeyStore& s)  {return KeyStoreWriter(s, *this);}
        KeyStoreWriter operator() (KeyStore* s) {return KeyStoreWriter(*s, *this);}

        Database* database() const          {return &_db;}
        state state() const                 {return _state;}

        /** Tells the Transaction that it should rollback, not commit, when exiting scope. */
        void abort()                        {if (_state != kNoOp) _state = kAbort;}

        /** Force the database write-ahead log to be completely flushed on commit. */
        void flushWAL()                     {if (_state == kCommit) _state = kCommitManualWALFlush;}

        void check(fdb_status status);

        /** Deletes the doc, and increments the database's purgeCount */
        bool del(slice key);
        bool del(Document &doc);

    private:
        friend class Database;
        Transaction(Database*, bool begin);
        Transaction(const Transaction&) = delete;

        Database& _db;
        enum state _state;
    };
    
}

#endif /* defined(__CBForest__Database__) */
