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
        void reopen() override;
        void compact() override;

        static void shutdown() { }

        operator SQLite::Database&() {return *_sqlDb;}

        std::vector<std::string> allKeyStoreNames() override;
        bool keyStoreExists(const std::string &name);

        class Factory : public DataFile::Factory {
        public:
            virtual const char* cname() override {return "SQLite";}
            virtual std::string filenameExtension() override {return ".sqlite3";}
            virtual bool encryptionEnabled(EncryptionAlgorithm) override;
            virtual SQLiteDataFile* openFile(const FilePath &, const Options* =nullptr) override;
            virtual bool deleteFile(const FilePath &path, const Options* =nullptr) override;
        };

        static Factory& factory();
        
    protected:
        void rekey(EncryptionAlgorithm, slice newKey) override;
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*, bool commit) override;
        KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) override;
        void deleteKeyStore(const std::string &name) override;

        sequence lastSequence(const std::string& keyStoreName) const;
        void setLastSequence(SQLiteKeyStore&, sequence);

        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   const char *sql) const;
        int exec(const std::string &sql);
        void registerFleeceFunctions();

    private:
        friend class SQLiteKeyStore;

        bool decrypt();

        std::unique_ptr<SQLite::Database>    _sqlDb;         // SQLite database object
        std::unique_ptr<SQLite::Transaction> _transaction;   // Current SQLite transaction
        std::unique_ptr<SQLite::Statement>   _getLastSeqStmt, _setLastSeqStmt;
        bool _registeredFleeceFunctions {false};
    };

}

