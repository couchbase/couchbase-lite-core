//
// SQLiteDataFile.hh
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

#include "DataFile.hh"
#include "QueryParser.hh"
#include "IndexSpec.hh"
#include "UnicodeCollator.hh"
#include <memory>
#include <optional>
#include <utility>
#include <utility>
#include <vector>

namespace SQLite {
    class Database;
    class Statement;
    class Transaction;
}  // namespace SQLite

namespace litecore {

    class SQLiteKeyStore;
    struct SQLiteIndexSpec;

    /** SQLite implementation of DataFile. */
    class SQLiteDataFile final
        : public DataFile
        , public QueryParser::Delegate {
      public:
        SQLiteDataFile(const FilePath& path, DataFile::Delegate* delegate, const Options*);
        ~SQLiteDataFile() override;

        bool isOpen() const noexcept override;

        uint64_t fileSize() override;
        void     optimize() noexcept;
        void     _optimize();
        void     vacuum(bool always) noexcept;
        void     _vacuum(bool always);
        void     integrityCheck();
        void     maintenance(MaintenanceType) override;

        static void shutdown() {}

        operator SQLite::Database&() { return *_sqlDb; }

        std::vector<std::string> allKeyStoreNames() const override;
        bool                     keyStoreExists(const std::string& name) const override;
        void                     deleteKeyStore(const std::string& name) override;

        static SQLiteKeyStore* asSQLiteKeyStore(KeyStore* ks);

        KeyStore& keyStoreFromTable(slice tableName);

        static bool tableNameIsCollection(slice tableName);
        static bool keyStoreNameIsCollection(slice ksName);

        [[nodiscard]] bool getSchema(const std::string& name, const std::string& type, const std::string& tableName,
                                     std::string& outSQL) const;
        [[nodiscard]] bool schemaExistsWithSQL(const std::string& name, const std::string& type,
                                               const std::string& tableName, const std::string& sql) const;

        fleece::alloc_slice rawQuery(const std::string& query) override;

        class Factory final : public DataFile::Factory {
          public:
            Factory();

            const char* cname() override { return "SQLite"; }

            std::string filenameExtension() override { return ".sqlite3"; }

            bool            encryptionEnabled(EncryptionAlgorithm) override;
            SQLiteDataFile* openFile(const FilePath&, DataFile::Delegate*, const Options* = nullptr) override;

          protected:
            bool _deleteFile(const FilePath& path, const Options* = nullptr) override;
        };

        static Factory& sqliteFactory();

        Factory& factory() const override { return SQLiteDataFile::sqliteFactory(); };

        /// Get an index's row count, and/or all its rows. Supports value and vector indexes.
        /// @warning For debugging/troubleshooting only!
        /// @param name  The name of the index
        /// @param outRowCount  On return, the number of rows will be stored here.
        /// @param outRows  If non-NULL, an encoded Fleece array of arrays will be stored here.
        ///                 Each array item is an index row; its items are its column values.
        void inspectIndex(slice name, int64_t& outRowCount, alloc_slice* outRows = nullptr);

        Retained<Query> compileQuery(slice expression, QueryLanguage, KeyStore*) override;

        /// Sets the directory where SQLite extensions can be found (i.e. VectorSearch)
        static void setExtensionPath(string);

        // QueryParser::delegate:
        bool        tableExists(const std::string& tableName) const override;
        string      collectionTableName(const string& collection, DeletionStatus) const override;
        string      auxiliaryTableName(const string& onTable, slice typeSeparator, const string& property) const;
        std::string FTSTableName(const string& collection, const std::string& property) const override;
        std::string unnestedTableName(const string& collection, const std::string& property) const override;
#ifdef COUCHBASE_ENTERPRISE
        std::string predictiveTableName(const string& collection, const std::string& property) const override;
        std::string vectorTableName(const string& collection, const std::string& property) const override;
#endif

      protected:
        std::string loggingClassName() const override { return "DB"; }

        void      logKeyStoreOp(SQLiteKeyStore&, const char* op, slice key);
        void      _close(bool forDelete) override;
        void      reopen() override;
        void      rekey(EncryptionAlgorithm, slice newKey) override;
        void      _beginTransaction(ExclusiveTransaction*) override;
        void      _endTransaction(ExclusiveTransaction*, bool commit) override;
        void      beginReadOnlyTransaction() override;
        void      endReadOnlyTransaction() override;
        KeyStore* newKeyStore(const std::string& name, KeyStore::Capabilities) override;

        sequence_t lastSequence(const std::string& keyStoreName) const;
        void       setLastSequence(SQLiteKeyStore&, sequence_t);
        uint64_t   purgeCount(const std::string& keyStoreName) const;
        void       setPurgeCount(SQLiteKeyStore&, uint64_t);

        std::unique_ptr<SQLite::Statement> compile(const char* sql) const;
        void                               compileCached(unique_ptr<SQLite::Statement>&, const char* sql) const;
        int                                exec(const std::string& sql);
        int                                execWithLock(const std::string& sql);
        int64_t                            intQuery(const char* query);
        void                               optimizeAndVacuum();

        // Indexes:
        bool createIndex(const litecore::IndexSpec& spec, SQLiteKeyStore* keyStore, const std::string& indexTableName,
                         const std::string& indexSQL);
        void deleteIndex(const SQLiteIndexSpec&);
        std::optional<SQLiteIndexSpec> getIndex(slice name);
        std::vector<SQLiteIndexSpec>   getIndexes(const KeyStore*);
        void                           setIndexSequences(slice name, slice sequencesJSON);
        void inspectVectorIndex(SQLiteIndexSpec const&, int64_t& outRowCount, alloc_slice* outRows);

      private:
        friend class SQLiteKeyStore;
        friend class SQLiteQuery;
        friend class LazyIndex;

        // SQLite schema versioning (values of `pragma user_version`)
        enum class SchemaVersion {
            None        = 0,    // Newly created database
            MinReadable = 201,  // Cannot open earlier versions than this (CBL 2.0)

            WithIndexTable = 301,  // Added 'indexes' table (CBL 2.5)
            WithPurgeCount = 302,  // Added 'purgeCnt' column to KeyStores (CBL 2.7)

            WithNewDocs = 400,  // New document/revision storage (CBL 3.0)

            WithDeletedTable = 500,  // Added 'deleted' KeyStore for deleted docs (CBL 3.0?)
            MaxReadable      = 599,  // Cannot open versions newer than this

            Current = WithDeletedTable
        };

        void reopenSQLiteHandle();
        void ensureSchemaVersionAtLeast(SchemaVersion);
        bool upgradeSchema(SchemaVersion minVersion, const char* what, function_ref<void()>);
        void migrateDeletedDocs();
        void decrypt();
        bool _decrypt(EncryptionAlgorithm, slice key);
        int  _exec(const std::string& sql);

        bool                         indexTableExists() const;
        void                         ensureIndexTableExists();
        void                         registerIndex(const litecore::IndexSpec&, const std::string& keyStoreName,
                                                   const std::string& indexTableName);
        void                         unregisterIndex(slice indexName);
        void                         garbageCollectIndexTable(const std::string& tableName);
        SQLiteIndexSpec              specFromStatement(SQLite::Statement& stmt);
        std::vector<SQLiteIndexSpec> getIndexesOldStyle(const KeyStore* store = nullptr);


        unique_ptr<SQLite::Database>          _sqlDb;  // SQLite database object
        std::unique_ptr<SQLiteKeyStore>       _realDefaultKeyStore;
        mutable unique_ptr<SQLite::Statement> _getLastSeqStmt, _setLastSeqStmt;
        mutable unique_ptr<SQLite::Statement> _getPurgeCntStmt, _setPurgeCntStmt;
        CollationContextVector                _collationContexts;
        SchemaVersion                         _schemaVersion{SchemaVersion::None};
    };

    struct SQLiteIndexSpec : public IndexSpec {
        SQLiteIndexSpec(const std::string& name, IndexSpec::Type type, alloc_slice expressionJSON,
                        QueryLanguage language, Options options, std::string ksName, std::string itName)
            : IndexSpec(name, type, std::move(expressionJSON), language, std::move(options))
            , keyStoreName(std::move(ksName))
            , indexTableName(std::move(itName)) {}

        std::string const keyStoreName;      ///< Name of KeyStore
        std::string const indexTableName;    ///< Name of SQLite table containing index (if any)
        alloc_slice       indexedSequences;  ///< Sequences that have been indexed; a JSON SequenceSet
    };


}  // namespace litecore
