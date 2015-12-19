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

        std::string filename() const;
        info getInfo() const;
        config getConfig()                      {return _config;}

        bool isReadOnly() const;

        void deleteDatabase()                   {deleteDatabase(false);}
        void erase()                            {deleteDatabase(true);}

        /** Deletes a database that isn't open. */
        static void deleteDatabase(std::string path, const config&);

        void compact();
        bool isCompacting() const               {return _isCompacting;}
        static bool isAnyCompacting();
        void setCompactionMode(fdb_compaction_mode_t);

        static void (*onCompactCallback)(Database* db, bool compacting);

        void rekey(const fdb_encryption_key&);

        /** Records a commit before the transaction exits scope. Not normally needed. */
        void commit();

        /** The Database's default key-value store. (You can also just use the Database
            instance directly as a KeyStore since it inherits from it.) */
        KeyStore defaultKeyStore() const        {return *this;}

        bool contains(KeyStore&) const;

        void closeKeyStore(std::string name);
        void deleteKeyStore(std::string name);

    protected:
        virtual void deleted();

        virtual bool onCompact(fdb_compaction_status status,
                               const char *kv_store_name,
                               fdb_doc *doc,
                               uint64_t lastOldFileOffset,
                               uint64_t lastNewFileOffset);

    private:
        class File;
        friend class KeyStore;
        friend class Transaction;
        fdb_kvs_handle* openKVS(std::string name) const;
        void beginTransaction(Transaction*);
        void endTransaction(Transaction*);
        void deleteDatabase(bool andReopen);
        void reopen(std::string path);

        Database(const Database&);              // forbidden
        Database& operator=(const Database&);   // forbidden

        static fdb_compact_decision compactionCallback(fdb_file_handle *fhandle,
                                                       fdb_compaction_status status,
                                                       const char *kv_store_name,
                                                       fdb_doc *doc,
                                                       uint64_t last_oldfile_offset,
                                                       uint64_t last_newfile_offset,
                                                       void *ctx);
        File* _file;
        config _config;
        fdb_file_handle* _fileHandle;
        std::unordered_map<std::string, fdb_kvs_handle*> _kvHandles;
        bool _isCompacting;
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
            kCommit
        };

        Transaction(Database*);
        ~Transaction();

        /** Converts a KeyStore to a KeyStoreWriter to allow write access. */
        KeyStoreWriter operator() (KeyStore s)  {return KeyStoreWriter(s, *this);}
        KeyStoreWriter operator() (KeyStore* s) {return KeyStoreWriter(*s, *this);}

        Database* database() const          {return &_db;}
        state state() const                 {return _state;}

        /** Tells the Transaction that it should rollback, not commit, when exiting scope. */
        void abort()                        {if (_state != kNoOp) _state = kAbort;}

        void check(fdb_status status);

    private:
        friend class Database;
        Transaction(Database*, bool begin);
        Transaction(const Transaction&); // forbidden

        Database& _db;
        enum state _state;
    };
    
}

#endif /* defined(__CBForest__Database__) */
