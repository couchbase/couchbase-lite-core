//
// SQLiteKeyStore.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "KeyStore.hh"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fleece::impl {
    class ArrayIterator;
    class Value;
}
namespace SQLite {
    class Column;
    class Statement;
}


namespace litecore {   

    class SQLiteDataFile;
    

    namespace RecordColumn {
        /// The order of result columns in a SELECT statement that returns a record.
        /// SQLiteKeyStore::setRecordMetaAndBody assumes the statement adheres to this.
        enum {
            Sequence = 0, RawFlags, Key, Version, BodyOrSize, ExtraOrSize, Expiration
        };
    }


    /** SQLite implementation of KeyStore; corresponds to a SQL table. */
    class SQLiteKeyStore final : public KeyStore {
    public:
        using KeyStore::get; // GCC gets confused by the overloaded virtual functions in KeyStore

        uint64_t recordCount() const override;
        sequence_t lastSequence() const override;
        uint64_t purgeCount() const override;

        std::string tableName() const                                       {return "kv_" + name();}

        bool read(Record &rec, ReadBy, ContentOption) const override;

        sequence_t set(const RecordUpdate&, bool updateSequence, ExclusiveTransaction&) override;
        void setKV(slice key, slice version, slice value, ExclusiveTransaction&) override;

        bool del(slice key, ExclusiveTransaction&, sequence_t s = 0) override;

        bool setDocumentFlag(slice key, sequence_t, DocumentFlags, ExclusiveTransaction&) override;

        void moveTo(slice key, KeyStore &dst, ExclusiveTransaction&, slice newKey = nullslice) override;

        void erase() override;

        virtual bool setExpiration(slice key, expiration_t) override;
        virtual expiration_t getExpiration(slice key) override;
        virtual expiration_t nextExpiration() override;
        virtual unsigned expireRecords(std::optional<ExpirationCallback>) override;

        bool supportsIndexes(IndexSpec::Type t) const override               {return true;}
        bool createIndex(const IndexSpec&) override;
        bool createIndex(const IndexSpec&, ExclusiveTransaction&) override;

        void deleteIndex(slice name) override;
        void deleteIndex(slice name, ExclusiveTransaction &t) override;
        std::vector<IndexSpec> getIndexes() const override;

        virtual std::vector<alloc_slice> withDocBodies(const std::vector<slice> &docIDs,
                                                       WithDocBodyCallback callback) override;

        void createSequenceIndex();
        void createConflictsIndex();
        void createBlobsIndex();

        /// Adds the `expiration` column to the table. Called only by SQLiteQuery.
        void addExpiration();

    protected:
        virtual bool mayHaveExpiration() override;
        RecordEnumerator::Impl* newEnumeratorImpl(bool bySequence,
                                                  sequence_t since,
                                                  RecordEnumerator::Options) override;

        std::unique_ptr<SQLite::Statement> compile(const char *sql) const;
        SQLite::Statement& compileCached(const std::string &sqlTemplate) const;

        void transactionWillEnd(bool commit);

        void close() override;
        void reopen() override;

        /// Updates a record's flags, version, body, extra from a statement whose column order
        /// matches the RecordColumn enum.
        static void setRecordMetaAndBody(Record &rec,
                                         SQLite::Statement &stmt,
                                         ContentOption,
                                         bool setKey,
                                         bool setSequence);

    private:
        friend class SQLiteDataFile;
        friend class SQLiteEnumerator;
        
        SQLiteKeyStore(SQLiteDataFile&, const std::string &name, KeyStore::Capabilities options);
        void createTable();
        SQLiteDataFile& db() const                    {return (SQLiteDataFile&)dataFile();}
        std::string subst(const char *sqlTemplate) const;
        void setLastSequence(sequence_t seq);
        void incrementPurgeCount();
        void createTrigger(std::string_view triggerName,
                           std::string_view triggerSuffix,
                           std::string_view operation,
                           std::string when,
                           std::string_view statements);
        bool createValueIndex(const IndexSpec&);
        bool createIndex(const IndexSpec&,
                              const std::string &sourceTableName,
                              fleece::impl::ArrayIterator &expressions);
        void _createFlagsIndex(const char *indexName NONNULL, DocumentFlags flag, bool &created);
        bool createFTSIndex(const IndexSpec&);
        bool createArrayIndex(const IndexSpec&);
        std::string createUnnestedTable(const fleece::impl::Value *arrayPath, const IndexSpec::Options*);

#ifdef COUCHBASE_ENTERPRISE
        bool createPredictiveIndex(const IndexSpec&);
        std::string createPredictionTable(const fleece::impl::Value *arrayPath, const IndexSpec::Options*);
        void garbageCollectPredictiveIndexes();
#endif

        using StatementCache = std::unordered_map<std::string,std::unique_ptr<SQLite::Statement>>;

        enum Existence : uint8_t { kNonexistent, kUncommitted, kCommitted };

        mutable std::mutex _stmtMutex;
        mutable StatementCache _stmtCache;
        bool _createdSeqIndex {false}, _createdConflictsIndex {false}, _createdBlobsIndex {false};
        bool _lastSequenceChanged {false};
        bool _purgeCountChanged {false};
        mutable bool _purgeCountValid {false};      // TODO: Use optional class from C++17
        mutable int64_t _lastSequence {-1};
        mutable std::atomic<uint64_t> _purgeCount {0};
        bool _hasExpirationColumn {false};
        bool _uncommittedExpirationColumn {false};
        Existence _existence;
    };

}
