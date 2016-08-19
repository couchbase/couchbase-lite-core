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

    using namespace std;

    class SQLiteKeyStore;


    /** SQLite implementation of Database. */
    class SQLiteDataFile : public DataFile {
    public:

        SQLiteDataFile(const string &path, const Options*);
        ~SQLiteDataFile();

        bool isOpen() const override;
        void close() override;
        void deleteDataFile() override;
        void reopen() override;
        void compact() override;

        static void deleteDataFile(const string &path);
        static void shutdown() { }

        operator SQLite::Database&() {return *_sqlDb;}

        vector<string> allKeyStoreNames() override;
        bool keyStoreExists(const string &name);

    protected:
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*) override;
        KeyStore* newKeyStore(const string &name, KeyStore::Capabilities) override;
        void deleteKeyStore(const string &name) override;

        sequence lastSequence(const string& keyStoreName) const;
        void setLastSequence(SQLiteKeyStore&, sequence);

        SQLite::Statement& compile(const unique_ptr<SQLite::Statement>& ref,
                                   string sql) const;
        int exec(const string &sql);

    private:
        friend class SQLiteKeyStore;

        bool decrypt();

        unique_ptr<SQLite::Database>    _sqlDb;         // SQLite database object
        unique_ptr<SQLite::Transaction> _transaction;   // Current SQLite transaction
        unique_ptr<SQLite::Statement>   _getLastSeqStmt, _setLastSeqStmt;
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
        SQLiteKeyStore(SQLiteDataFile&, const string &name, KeyStore::Capabilities options);
        SQLiteDataFile& db() const                    {return (SQLiteDataFile&)dataFile();}
        stringstream selectFrom(const DocEnumerator::Options &options);
        void writeSQLOptions(stringstream &sql, DocEnumerator::Options &options);
        void backupReplacedDoc(slice key);

        unique_ptr<SQLite::Statement> _docCountStmt;
        unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt, _getByOffStmt;
        unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        unique_ptr<SQLite::Statement> _setStmt, _backupStmt, _delByKeyStmt, _delBySeqStmt;
        bool                          _createdKeyIndex {false};     // Created by-key index yet?
        bool                          _createdSeqIndex {false};     // Created by-seq index yet?
    };

}


#endif /* SQLiteDataFile_hh */
