//
//  SQLiteKeyStore.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "KeyStore.hh"

namespace fleece {
    class Value;
}
namespace SQLite {
    class Column;
    class Statement;
}


namespace litecore {

    class SQLiteDataFile;
    

    /** SQLite implementation of KeyStore; corresponds to a SQL table. */
    class SQLiteKeyStore : public KeyStore {
    public:
        uint64_t recordCount() const override;
        sequence lastSequence() const override;

        Record get(sequence, ContentOptions) const override;
        bool read(Record &rec, ContentOptions options) const override;
        Record getByOffsetNoErrors(uint64_t offset, sequence) const override;

        setResult set(slice key, slice meta, slice value, Transaction&) override;

        void erase() override;

        bool supportsIndexes(IndexType t) const override               {return t == kValueIndex;}
        void createIndex(const std::string &propertyPath,
                         IndexType =kValueIndex,
                         const IndexOptions* = nullptr) override;
        void deleteIndex(const std::string &propertyPath, IndexType =kValueIndex) override;
        bool hasIndex(const std::string &propertyPath, IndexType =kValueIndex);

    protected:
        std::string tableName() const                       {return std::string("kv_") + name();}
        bool _del(slice key, Transaction &t) override       {return _del(key, 0, t);}
        bool _del(sequence s, Transaction &t) override      {return _del(nullslice, s, t);}
        bool _del(slice key, sequence s, Transaction&);

        RecordEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, RecordEnumerator::Options&) override;
        RecordEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max, RecordEnumerator::Options&) override;
        Query* compileQuery(slice expression) override;

        SQLite::Statement* compile(const std::string &sql) const;
        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   const char *sqlTemplate) const;

        void transactionWillEnd(bool commit);

        void close() override;

        static slice columnAsSlice(const SQLite::Column &col);
        static void setRecordMetaAndBody(Record &rec,
                                         SQLite::Statement &stmt,
                                         ContentOptions options);

    private:
        friend class SQLiteDataFile;
        friend class SQLiteEnumerator;
        friend class SQLiteQuery;
        
        SQLiteKeyStore(SQLiteDataFile&, const std::string &name, KeyStore::Capabilities options);
        SQLiteDataFile& db() const                    {return (SQLiteDataFile&)dataFile();}
        std::string subst(const char *sqlTemplate) const;
        void selectFrom(std::stringstream& in, const RecordEnumerator::Options &options);
        void writeSQLOptions(std::stringstream &sql, RecordEnumerator::Options &options);
        void setLastSequence(sequence seq);
        std::string SQLIndexName(const std::string &propertyPath, const char *suffix = NULL);
        std::string SQLFTSTableName(const std::string &propertyPath);

        std::unique_ptr<SQLite::Statement> _recCountStmt;
        std::unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt, _getByOffStmt;
        std::unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        std::unique_ptr<SQLite::Statement> _setStmt, _backupStmt, _delByKeyStmt, _delBySeqStmt;
        bool _createdSeqIndex {false};     // Created by-seq index yet?
        bool _lastSequenceChanged {false};
        int64_t _lastSequence {-1};
    };

}
