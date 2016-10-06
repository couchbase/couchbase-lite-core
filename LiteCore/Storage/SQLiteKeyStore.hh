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
        uint64_t documentCount() const override;
        sequence lastSequence() const override;

        Document get(sequence, ContentOptions) const override;
        bool read(Document &doc, ContentOptions options) const override;
        Document getByOffsetNoErrors(uint64_t offset, sequence) const override;

        setResult set(slice key, slice meta, slice value, Transaction&) override;

        void erase() override;

        bool supportsIndexes() const override               {return true;}
        void createIndex(const std::string &propertyPath) override;
        void deleteIndex(const std::string &propertyPath) override;

    protected:
        bool _del(slice key, Transaction &t) override       {return _del(key, 0, t);}
        bool _del(sequence s, Transaction &t) override      {return _del(slice::null, s, t);}
        bool _del(slice key, sequence s, Transaction&);

        DocEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey, DocEnumerator::Options&) override;
        DocEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max, DocEnumerator::Options&) override;
        Query* compileQuery(slice selectorExpression, slice sortExpression) override;

        SQLite::Statement* compile(const std::string &sql) const;
        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   const char *sqlTemplate) const;

        void transactionWillEnd(bool commit);

        void close() override;

        static slice columnAsSlice(const SQLite::Column &col);
        static void setDocMetaAndBody(Document &doc,
                                      SQLite::Statement &stmt,
                                      ContentOptions options);

    private:
        friend class SQLiteDataFile;
        friend class SQLiteIterator;
        friend class SQLiteQuery;
        
        SQLiteKeyStore(SQLiteDataFile&, const std::string &name, KeyStore::Capabilities options);
        SQLiteDataFile& db() const                    {return (SQLiteDataFile&)dataFile();}
        std::string subst(const char *sqlTemplate) const;
        std::stringstream selectFrom(const DocEnumerator::Options &options);
        void writeSQLOptions(std::stringstream &sql, DocEnumerator::Options &options);
        void setLastSequence(sequence seq);
        void writeSQLIndexName(const std::string &propertyPath, std::stringstream &sql);

        std::unique_ptr<SQLite::Statement> _docCountStmt;
        std::unique_ptr<SQLite::Statement> _getByKeyStmt, _getMetaByKeyStmt, _getByOffStmt;
        std::unique_ptr<SQLite::Statement> _getBySeqStmt, _getMetaBySeqStmt;
        std::unique_ptr<SQLite::Statement> _setStmt, _backupStmt, _delByKeyStmt, _delBySeqStmt;
        bool _createdSeqIndex {false};     // Created by-seq index yet?
        bool _lastSequenceChanged {false};
        int64_t _lastSequence {-1};
    };

}
