//
// SQLiteKeyStore.hh
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
#include "KeyStore.hh"
#include "QueryParser.hh"
#include "FleeceImpl.hh"

namespace SQLite {
    class Column;
    class Statement;
}


namespace litecore {

    class SQLiteDataFile;
    

    /** SQLite implementation of KeyStore; corresponds to a SQL table. */
    class SQLiteKeyStore : public KeyStore, public QueryParser::delegate {
    public:
        uint64_t recordCount() const override;
        sequence_t lastSequence() const override;

        Record get(sequence_t) const override;
        bool read(Record &rec, ContentOptions options) const override;

        sequence_t set(slice key, slice meta, slice value, DocumentFlags,
                       Transaction&,
                       const sequence_t *replacingSequence =nullptr,
                       bool newSequence =true) override;

        bool del(slice key, Transaction&, sequence_t s) override;

        bool setDocumentFlag(slice key, sequence_t, DocumentFlags, Transaction&) override;

        void erase() override;

        bool supportsIndexes(IndexType t) const override               {return true;}
        bool createIndex(slice name,
                         slice expressionJSON,
                         IndexType =kValueIndex,
                         const IndexOptions* = nullptr) override;

        void deleteIndex(slice name) override;
        alloc_slice getIndexes() const override;

        void createSequenceIndex();

        // QueryParser::delegate:
        virtual std::string tableName() const override  {return std::string("kv_") + name();}
        virtual std::string FTSTableName(const std::string &property) const override;
        virtual std::string unnestedTableName(const std::string &property) const override;
        virtual bool tableExists(const std::string &tableName) const override;


    protected:
        RecordEnumerator::Impl* newEnumeratorImpl(bool bySequence,
                                                  sequence_t since,
                                                  RecordEnumerator::Options) override;
        Retained<Query> compileQuery(slice expression) override;

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
        void selectFrom(std::stringstream& in, const RecordEnumerator::Options options);
        void writeSQLOptions(std::stringstream &sql, RecordEnumerator::Options options);
        void setLastSequence(sequence_t seq);
        void createTrigger(const std::string &triggerName,
                           const char *triggerSuffix,
                           const char *operation,
                           const char *when,
                           const std::string &statements);
        void dropTrigger(const std::string &name, const char *suffix);
        bool createValueIndex(IndexType, const std::string &sourceTableName,
                              const std::string &indexName,
                              fleece::impl::Array::iterator &expressions,
                              const IndexOptions *options);
        bool createFTSIndex(std::string, const fleece::impl::Array *params, const IndexOptions*);
        bool createArrayIndex(std::string, const fleece::impl::Array *params, const IndexOptions*);
        std::string createUnnestedTable(const fleece::impl::Value *arrayPath, const IndexOptions*);
        bool _schemaExistsWithSQL(const std::string &name, const std::string &type,
                                  const std::string &tableName, const std::string &sql);
        void _sqlDeleteIndex(const std::string &name);
        void garbageCollectArrayIndexes();

        std::unique_ptr<SQLite::Statement> _recCountStmt;
        std::unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt, _getByOffStmt;
        std::unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        std::unique_ptr<SQLite::Statement> _setStmt, _insertStmt, _replaceStmt, _updateBodyStmt;
        std::unique_ptr<SQLite::Statement> _backupStmt, _delByKeyStmt, _delBySeqStmt, _delByBothStmt;
        std::unique_ptr<SQLite::Statement> _setFlagStmt;
        bool _createdSeqIndex {false};     // Created by-seq index yet?
        bool _lastSequenceChanged {false};
        int64_t _lastSequence {-1};
    };

}
