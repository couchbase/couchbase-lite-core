//
//  SQLiteDataFile.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/21/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "DataFile.hh"

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
        void deleteDataFile() override;
        void compact() override;

        static void shutdown() { }

        operator SQLite::Database&() {return *_sqlDb;}

        std::vector<std::string> allKeyStoreNames() override;
        bool keyStoreExists(const std::string &name);
        bool tableExists(const std::string &name) const;

        class Factory : public DataFile::Factory {
        public:
            Factory();
            virtual const char* cname() override {return "SQLite";}
            virtual std::string filenameExtension() override {return ".sqlite3";}
            virtual bool encryptionEnabled(EncryptionAlgorithm) override;
            virtual SQLiteDataFile* openFile(const FilePath &, const Options* =nullptr) override;
            virtual bool deleteFile(const FilePath &path, const Options* =nullptr) override;
        };

        static Factory& factory();

    protected:
        void reopen() override;
        void rekey(EncryptionAlgorithm, slice newKey) override;
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*, bool commit) override;
        void beginReadOnlyTransaction() override;
        void endReadOnlyTransaction() override;
        KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) override;
        void deleteKeyStore(const std::string &name) override;

        sequence_t lastSequence(const std::string& keyStoreName) const;
        void setLastSequence(SQLiteKeyStore&, sequence_t);

        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   const char *sql) const;
        int exec(const std::string &sql);
        int execWithLock(const std::string &sql);
        int64_t intQuery(const char *query);
        void maybeVacuum();

    private:
        friend class SQLiteKeyStore;

        bool decrypt();

        std::unique_ptr<SQLite::Database>    _sqlDb;         // SQLite database object
        std::unique_ptr<SQLite::Statement>   _getLastSeqStmt, _setLastSeqStmt;
    };

}

