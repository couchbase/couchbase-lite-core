//
//  SQLiteDatabase.hh
//  CBNano
//
//  Created by Jens Alfke on 7/21/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef SQLiteDatabase_hh
#define SQLiteDatabase_hh

#include "Database.hh"
#include "SQLiteCpp/Database.h"
#include "SQLiteCpp/Transaction.h"

namespace SQLite {
    class Statement;
}


namespace cbforest {

    using namespace std;

    class SQLiteKeyStore;


    class SQLiteDatabase : public Database {
    public:

        SQLiteDatabase(const string &path, const Options*);
        ~SQLiteDatabase();

        bool isOpen() const override;
        void close() override;
        void deleteDatabase() override;
        void reopen() override;
        void compact() override;

        static void deleteDatabase(const string &path);

        operator SQLite::Database&() {return *_sqlDb;}

        vector<string> allKeyStoreNames() override;
        bool keyStoreExists(const string &name);

    protected:
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*) override;
        KeyStore* newKeyStore(const string &name, KeyStore::Options) override;
        void deleteKeyStore(const string &name) override;

        sequence lastSequence(const string& keyStoreName) const;
        void setLastSequence(SQLiteKeyStore&, sequence);

        SQLite::Statement& compile(const unique_ptr<SQLite::Statement>& ref,
                                   string sql) const;
        int exec(const string &sql);

    private:
        friend class SQLiteKeyStore;

        unique_ptr<SQLite::Database> _sqlDb;
        unique_ptr<SQLite::Transaction> _transaction;
        unique_ptr<SQLite::Statement> _getLastSeqStmt, _setLastSeqStmt;
    };



    class SQLiteKeyStore : public KeyStore {
    public:
        uint64_t documentCount() const override;
        sequence lastSequence() const override              {return db().lastSequence(_name);}

        Document get(sequence, ContentOptions) const override;
        bool read(Document &doc, ContentOptions options) const override;

        sequence set(slice key, slice meta, slice value, Transaction&) override;

        void erase() override;

    protected:
        bool _del(slice key, Transaction &t) override       {return _del(key, 0, t);}
        bool _del(sequence s, Transaction &t) override      {return _del(slice::null, s, t);}
        bool _del(slice key, sequence s, Transaction&);

        DocEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, DocEnumerator::Options&) override;
        DocEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max, DocEnumerator::Options&) override;

    private:
        friend class SQLiteDatabase;
        SQLiteKeyStore(SQLiteDatabase&, const string &name, KeyStore::Options options);
        SQLiteDatabase& db() const                    {return (SQLiteDatabase&)database();}
        stringstream selectFrom(const DocEnumerator::Options &options);
        void writeSQLOptions(stringstream &sql, DocEnumerator::Options &options);

        unique_ptr<SQLite::Statement> _docCountStmt;
        unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt;
        unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        unique_ptr<SQLite::Statement> _setStmt, _delByKeyStmt, _delBySeqStmt;
        bool _createdKeyIndex {false};
        bool _createdSeqIndex {false};
    };



    class SQLiteDatabaseFactory : public DatabaseFactory {
    public:
        virtual Database* newDatabase(const string &path,
                                      const Database::Options* options =nullptr) override
        {
            return new SQLiteDatabase(path, options);
        }

        virtual std::string name() const override {
            return std::string("SQLite");
        }
        virtual ~SQLiteDatabaseFactory() { }
    };
    
}


#endif /* SQLiteDatabase_hh */
