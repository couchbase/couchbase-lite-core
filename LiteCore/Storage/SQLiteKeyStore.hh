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
    class Array;
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
        sequence_t lastSequence() const override;

        Record get(sequence_t, ContentOptions) const override;
        bool read(Record &rec, ContentOptions options) const override;

        sequence_t set(slice key, slice meta, slice value, DocumentFlags,
                       Transaction&, const sequence_t *replacingSequence =nullptr) override;

        void erase() override;

        bool supportsIndexes(IndexType t) const override               {return t == kValueIndex;}
        void createIndex(slice expressionJSON,
                         IndexType =kValueIndex,
                         const IndexOptions* = nullptr) override;
        void deleteIndex(slice expressionJSON, IndexType =kValueIndex) override;
        bool hasIndex(slice expressionJSON, IndexType =kValueIndex);

    protected:
        std::string tableName() const                       {return std::string("kv_") + name();}
        bool _del(slice key, sequence_t s, Transaction&) override;

        RecordEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, RecordEnumerator::Options&) override;
        RecordEnumerator::Impl* newEnumeratorImpl(sequence_t min, sequence_t max, RecordEnumerator::Options&) override;
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
        void setLastSequence(sequence_t seq);
        std::string SQLIndexName(const fleece::Array*, IndexType, bool quoted =false);

        std::unique_ptr<SQLite::Statement> _recCountStmt;
        std::unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt, _getByOffStmt;
        std::unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        std::unique_ptr<SQLite::Statement> _setStmt, _insertStmt, _replaceStmt;
        std::unique_ptr<SQLite::Statement> _backupStmt, _delByKeyStmt, _delBySeqStmt, _delByBothStmt;
        bool _createdSeqIndex {false};     // Created by-seq index yet?
        bool _lastSequenceChanged {false};
        int64_t _lastSequence {-1};
    };

}
