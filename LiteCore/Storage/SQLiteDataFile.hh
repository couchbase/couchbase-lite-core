//
// SQLiteDataFile.hh
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

#include "DataFile.hh"
#include "IndexSpec.hh"
#include "UnicodeCollator.hh"
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
    class SQLiteDataFile : public DataFile {
    public:

        SQLiteDataFile(const FilePath &path, Delegate *delegate, const Options*);
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

        std::vector<std::string> allKeyStoreNames() /*override*/;
        bool keyStoreExists(const std::string &name);
        bool tableExists(const std::string &name) const;
        bool getSchema(const std::string &name, const std::string &type,
                       const std::string &tableName, std::string &outSQL) const;
        bool schemaExistsWithSQL(const std::string &name, const std::string &type,
                                 const std::string &tableName, const std::string &sql);

        fleece::alloc_slice rawQuery(const std::string &query) override;

        class Factory : public DataFile::Factory {
        public:
            Factory();
            virtual const char* cname() override {return "SQLite";}
            virtual std::string filenameExtension() override {return ".sqlite3";}
            virtual bool encryptionEnabled(EncryptionAlgorithm) override;
            virtual SQLiteDataFile* openFile(const FilePath &, Delegate*, const Options* =nullptr) override;
        protected:
            virtual bool _deleteFile(const FilePath &path, const Options* =nullptr) override;
        };

        static Factory& sqliteFactory();
        virtual Factory& factory() const override   {return SQLiteDataFile::sqliteFactory();};

        // Get an index's row count, and/or all its rows. For debugging/troubleshooting only!
        void inspectIndex(slice name,
                          int64_t &outRowCount,
                          alloc_slice *outRows =nullptr);

    protected:
        std::string loggingClassName() const override       {return "DB";}
        void logKeyStoreOp(SQLiteKeyStore&, const char *op, slice key);
        void _close(bool forDelete) override;
        void reopen() override;
        void rekey(EncryptionAlgorithm, slice newKey) override;
        void _beginTransaction(Transaction*) override;
        void _endTransaction(Transaction*, bool commit) override;
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

        SQLite::Statement& compile(const unique_ptr<SQLite::Statement>& ref,
                                   const char *sql) const;
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

        // SQLite schema versioning (values of `pragma user_version`)
        enum class SchemaVersion {
            None            = 0,    // Newly created database
            MinReadable     = 201,  // Cannot open earlier versions than this (CBL 2.0)
            MaxReadable     = 499,  // Cannot open versions newer than this

            WithIndexTable  = 301,  // Added 'indexes' table (CBL 2.5)
            WithPurgeCount  = 302,  // Added 'purgeCnt' column to KeyStores (CBL 2.7)

            WithNewDocs     = 400,  // New document/revision storage (CBL 3.0)

            Current = WithNewDocs
        };

        void reopenSQLiteHandle();
        void ensureSchemaVersionAtLeast(SchemaVersion);
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
        unique_ptr<SQLite::Statement>   _getLastSeqStmt, _setLastSeqStmt;
        unique_ptr<SQLite::Statement>   _getPurgeCntStmt, _setPurgeCntStmt;
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

