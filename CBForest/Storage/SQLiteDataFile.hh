//
//  SQLiteDataFile.hh
//  CBForest
//
//  Created by Jens Alfke on 7/21/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef SQLiteDatabase_hh
#define SQLiteDatabase_hh

#include "DataFile.hh"

namespace SQLite {
    class Database;
    class Statement;
    class Transaction;
}


namespace cbforest {

    class SQLiteKeyStore;


    /** SQLite implementation of Database. */
    class SQLiteDataFile : public DataFile {
    public:

        static const char *kFilenameExtension;

        SQLiteDataFile(const FilePath &path, const Options*);
        ~SQLiteDataFile();

        bool isOpen() const override;
        void close() override;
        void deleteDataFile() override;
        void reopen() override;
        void compact() override;

        static void deleteDataFile(const FilePath &path);
        static void shutdown() { }

        operator SQLite::Database&() {return *_sqlDb;}

        std::vector<std::string> allKeyStoreNames() override;
        bool keyStoreExists(const std::string &name);

    protected:
        void rekey(EncryptionAlgorithm, slice newKey) override;
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*) override;
        KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) override;
        void deleteKeyStore(const std::string &name) override;

        sequence lastSequence(const std::string& keyStoreName) const;
        void setLastSequence(SQLiteKeyStore&, sequence);

        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   std::string sql) const;
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
        sequence lastSequence() const override              {return db().lastSequence(_name);}

        Document get(sequence, ContentOptions) const override;
        bool read(Document &doc, ContentOptions options) const override;
        Document getByOffsetNoErrors(uint64_t offset, sequence) const override;

        sequence set(slice key, slice meta, slice value, Transaction&) override;

        void erase() override;

    protected:
        bool _del(slice key, Transaction &t) override       {return _del(key, 0, t);}
        bool _del(sequence s, Transaction &t) override      {return _del(slice::null, s, t);}
        bool _del(slice key, sequence s, Transaction&);

        DocEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, DocEnumerator::Options&) override;
        DocEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max, DocEnumerator::Options&) override;

        void close() override;

    private:
        friend class SQLiteDataFile;
        SQLiteKeyStore(SQLiteDataFile&, const std::string &name, KeyStore::Capabilities options);
        SQLiteDataFile& db() const                    {return (SQLiteDataFile&)dataFile();}
        std::stringstream selectFrom(const DocEnumerator::Options &options);
        void writeSQLOptions(std::stringstream &sql, DocEnumerator::Options &options);
        void backupReplacedDoc(slice key);

        std::unique_ptr<SQLite::Statement> _docCountStmt;
        std::unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt, _getByOffStmt;
        std::unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        std::unique_ptr<SQLite::Statement> _setStmt, _backupStmt, _delByKeyStmt, _delBySeqStmt;
        bool _createdKeyIndex {false};     // Created by-key index yet?
        bool _createdSeqIndex {false};     // Created by-seq index yet?
    };

}


#endif /* SQLiteDataFile_hh */
