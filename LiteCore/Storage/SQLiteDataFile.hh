//
// SQLiteDataFile.hh
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

#include "DataFile.hh"
#include "UnicodeCollator.hh"

namespace SQLite {
    class Database;
    class Statement;
    class Transaction;
}


namespace litecore {

    class SQLiteKeyStore;


    /** SQLite implementation of Database. */
    class SQLiteDataFile : public DataFile {
    public:

        SQLiteDataFile(const FilePath &path, const Options*);
        ~SQLiteDataFile();

        bool isOpen() const noexcept override;
        void close() override;
        void compact() override;

        static void shutdown() { }

        operator SQLite::Database&() {return *_sqlDb;}

#if 0 //UNUSED:
        std::vector<std::string> allKeyStoreNames() override;
#endif
        bool keyStoreExists(const std::string &name);
        bool tableExists(const std::string &name) const;

        fleece::alloc_slice rawQuery(const std::string &query) override;

        class Factory : public DataFile::Factory {
        public:
            Factory();
            virtual const char* cname() override {return "SQLite";}
            virtual std::string filenameExtension() override {return ".sqlite3";}
            virtual bool encryptionEnabled(EncryptionAlgorithm) override;
            virtual SQLiteDataFile* openFile(const FilePath &, const Options* =nullptr) override;
        protected:
            virtual bool _deleteFile(const FilePath &path, const Options* =nullptr) override;
        };

        static Factory& sqliteFactory();
        virtual Factory& factory() const override   {return SQLiteDataFile::sqliteFactory();};

    protected:
        void reopen() override;
        void rekey(EncryptionAlgorithm, slice newKey) override;
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*, bool commit) override;
        void beginReadOnlyTransaction() override;
        void endReadOnlyTransaction() override;
        KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) override;
#if ENABLE_DELETE_KEY_STORES
        void deleteKeyStore(const std::string &name) override;
#endif

        sequence_t lastSequence(const std::string& keyStoreName) const;
        void setLastSequence(SQLiteKeyStore&, sequence_t);

        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   const char *sql) const;
        int exec(const std::string &sql, LogLevel =LogLevel::Verbose);
        int execWithLock(const std::string &sql);
        int64_t intQuery(const char *query);
        void optimizeAndVacuum();

    private:
        friend class SQLiteKeyStore;

        bool decrypt();
        int _exec(const std::string &sql, LogLevel =LogLevel::Verbose);

        std::unique_ptr<SQLite::Database>    _sqlDb;         // SQLite database object
        std::unique_ptr<SQLite::Statement>   _getLastSeqStmt, _setLastSeqStmt;
        CollationContextVector _collationContexts;
    };

}

