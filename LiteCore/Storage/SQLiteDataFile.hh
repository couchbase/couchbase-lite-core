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
#include <vector>

namespace SQLite {
    class Database;
    class Statement;
    class Transaction;
}


namespace litecore {

    class SQLiteKeyStore;
    struct SQLiteIndexSpec;


    /** SQLite implementation of DataFile. */
    class SQLiteDataFile final : public DataFile, public QueryParser::Delegate {
    public:

        SQLiteDataFile(const FilePath &path, DataFile::Delegate *delegate, const Options*);
        ~SQLiteDataFile();

        bool isOpen() const noexcept override;

        uint64_t fileSize() override;
        void optimize() noexcept;
        void _optimize();
        void vacuum(bool always) noexcept;
        void _vacuum(bool always);
        void integrityCheck();
        void maintenance(MaintenanceType) override;

        static void shutdown() { }

        operator SQLite::Database&() {return *_sqlDb;}

        std::vector<std::string> allKeyStoreNames() const override;
        bool keyStoreExists(const std::string &name) const override;
        void deleteKeyStore(const std::string &name) override;

        KeyStore& keyStoreFromTable(slice tableName);

        SQLiteKeyStore* asSQLiteKeyStore(KeyStore *ks) const;

        static bool tableNameIsCollection(slice tableName);
        static bool keyStoreNameIsCollection(slice ksName);

        bool getSchema(const std::string &name, const std::string &type,
                       const std::string &tableName, std::string &outSQL) const;
        bool schemaExistsWithSQL(const std::string &name, const std::string &type,
                                 const std::string &tableName, const std::string &sql);

        fleece::alloc_slice rawQuery(const std::string &query) override;

        class Factory final : public DataFile::Factory {
        public:
            Factory();
            virtual const char* cname() override {return "SQLite";}
            virtual std::string filenameExtension() override {return ".sqlite3";}
            virtual bool encryptionEnabled(EncryptionAlgorithm) override;
            virtual SQLiteDataFile* openFile(const FilePath &, DataFile::Delegate*, const Options* =nullptr) override;
        protected:
            virtual bool _deleteFile(const FilePath &path, const Options* =nullptr) override;
        };

        static Factory& sqliteFactory();
        virtual Factory& factory() const override   {return SQLiteDataFile::sqliteFactory();};

        // Get an index's row count, and/or all its rows. For debugging/troubleshooting only!
        void inspectIndex(slice name,
                          int64_t &outRowCount,
                          alloc_slice *outRows =nullptr);

        Retained<Query> compileQuery(slice expression, QueryLanguage, KeyStore*) override;

    // QueryParser::delegate:
        virtual bool tableExists(const std::string &tableName) const override;
        virtual string collectionTableName(const string &collection, DeletionStatus) const override;
        virtual std::string FTSTableName(const string &collection, const std::string &property) const override;
        virtual std::string unnestedTableName(const string &collection, const std::string &property) const override;
#ifdef COUCHBASE_ENTERPRISE
        virtual std::string predictiveTableName(const string &collection, const std::string &property) const override;
#endif

    protected:
        std::string loggingClassName() const override       {return "DB";}
        void logKeyStoreOp(SQLiteKeyStore&, const char *op, slice key);
        void _close(bool forDelete) override;
        void reopen() override;
        void rekey(EncryptionAlgorithm, slice newKey) override;
        void _beginTransaction(ExclusiveTransaction*) override;
        void _endTransaction(ExclusiveTransaction*, bool commit) override;
        void beginReadOnlyTransaction() override;
        void endReadOnlyTransaction() override;
        KeyStore* newKeyStore(const std::string &name, KeyStore::Capabilities) override;
#if ENABLE_DELETE_KEY_STORES
        void deleteKeyStore(const std::string &name) override;
#endif

        sequence_t lastSequence(const std::string& keyStoreName) const;
        void setLastSequence(SQLiteKeyStore&, sequence_t);
        uint64_t purgeCount(const std::string& keyStoreName) const;
        void setPurgeCount(SQLiteKeyStore&, uint64_t);

        std::unique_ptr<SQLite::Statement> compile(const char *sql) const;
        void compileCached(unique_ptr<SQLite::Statement>&, const char *sql) const;
        int exec(const std::string &sql);
        int execWithLock(const std::string &sql);
        int64_t intQuery(const char *query);
        void optimizeAndVacuum();

        // Indexes:
        bool createIndex(const litecore::IndexSpec &spec,
                         SQLiteKeyStore *keyStore,
                         const std::string &indexTableName,
                         const std::string &indexSQL);
        void deleteIndex(const SQLiteIndexSpec&);
        std::optional<SQLiteIndexSpec> getIndex(slice name);
        std::vector<SQLiteIndexSpec> getIndexes(const KeyStore*);

    private:
        friend class SQLiteKeyStore;
        friend class SQLiteQuery;

        // SQLite schema versioning (values of `pragma user_version`)
        enum class SchemaVersion {
            None            = 0,    // Newly created database
            MinReadable     = 201,  // Cannot open earlier versions than this (CBL 2.0)

            WithIndexTable  = 301,  // Added 'indexes' table (CBL 2.5)
            WithPurgeCount  = 302,  // Added 'purgeCnt' column to KeyStores (CBL 2.7)

            WithNewDocs     = 400,  // New document/revision storage (CBL 3.0)

            WithDeletedTable= 500,  // Added 'deleted' KeyStore for deleted docs (CBL 3.0?)
            MaxReadable     = 599,  // Cannot open versions newer than this

            Current = WithDeletedTable
        };

        void reopenSQLiteHandle();
        void ensureSchemaVersionAtLeast(SchemaVersion);
        bool upgradeSchema(SchemaVersion minVersion, const char *what, function_ref<void()>);
        void migrateDeletedDocs();
        void decrypt();
        bool _decrypt(EncryptionAlgorithm, slice key);
        int _exec(const std::string &sql);

        bool indexTableExists();
        void ensureIndexTableExists();
        void registerIndex(const litecore::IndexSpec&,
                           const std::string &keyStoreName,
                           const std::string &indexTableName);
        void unregisterIndex(slice indexName);
        void garbageCollectIndexTable(const std::string &tableName);
        SQLiteIndexSpec specFromStatement(SQLite::Statement &stmt);
        std::vector<SQLiteIndexSpec> getIndexesOldStyle(const KeyStore *store =nullptr);

        unique_ptr<SQLite::Database>    _sqlDb;         // SQLite database object
        std::unique_ptr<SQLiteKeyStore> _realDefaultKeyStore;
        mutable unique_ptr<SQLite::Statement>   _getLastSeqStmt, _setLastSeqStmt;
        mutable unique_ptr<SQLite::Statement>   _getPurgeCntStmt, _setPurgeCntStmt;
        CollationContextVector          _collationContexts;
        SchemaVersion                   _schemaVersion {SchemaVersion::None};
    };


    struct SQLiteIndexSpec : public IndexSpec {
        SQLiteIndexSpec(const std::string &name,
                        IndexSpec::Type type,
                        alloc_slice expressionJSON,
                        const std::string &ksName,
                        const std::string &itName)
        :IndexSpec(name, type, expressionJSON)
        ,keyStoreName(ksName)
        ,indexTableName(itName)
        { }

        std::string const keyStoreName;
        std::string const indexTableName;
    };


}

