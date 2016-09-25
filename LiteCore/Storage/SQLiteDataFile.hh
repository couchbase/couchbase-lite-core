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

    private:
        friend class SQLiteKeyStore;

        bool encryptionEnabled();
        bool decrypt();

        std::unique_ptr<SQLite::Database>    _sqlDb;         // SQLite database object
        std::unique_ptr<SQLite::Transaction> _transaction;   // Current SQLite transaction
        std::unique_ptr<SQLite::Statement>   _getLastSeqStmt, _setLastSeqStmt;
    };



    /** SQLite implementation of KeyStore; corresponds to a SQL table. */
    class SQLiteKeyStore : public KeyStore {
    public:
        uint64_t documentCount() const override;
        sequence lastSequence() const override;

        Document get(sequence, ContentOptions) const override;
        bool read(Document &doc, ContentOptions options) const override;
        Document getByOffsetNoErrors(uint64_t offset, sequence) const override;

        setResult set(slice key, slice meta, slice value, Transaction&) override;

        void erase() override;

    protected:
        bool _del(slice key, Transaction &t) override       {return _del(key, 0, t);}
        bool _del(sequence s, Transaction &t) override      {return _del(slice::null, s, t);}
        bool _del(slice key, sequence s, Transaction&);

        DocEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, DocEnumerator::Options&) override;
        DocEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max, DocEnumerator::Options&) override;

        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   const char *sql) const;

        void transactionWillEnd(bool commit);

        void close() override;

    private:
        friend class SQLiteDataFile;
        SQLiteKeyStore(SQLiteDataFile&, const std::string &name, KeyStore::Capabilities options);
        SQLiteDataFile& db() const                    {return (SQLiteDataFile&)dataFile();}
        std::string subst(const char *sqlTemplate) const;
        std::stringstream selectFrom(const DocEnumerator::Options &options);
        void writeSQLOptions(std::stringstream &sql, DocEnumerator::Options &options);
        void setLastSequence(sequence seq);

        std::unique_ptr<SQLite::Statement> _docCountStmt;
        std::unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt, _getByOffStmt;
        std::unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        std::unique_ptr<SQLite::Statement> _setStmt, _backupStmt, _delByKeyStmt, _delBySeqStmt;
        bool _createdSeqIndex {false};     // Created by-seq index yet?
        bool _lastSequenceChanged {false};
        int64_t _lastSequence {-1};
    };

}

