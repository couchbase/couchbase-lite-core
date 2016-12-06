//
//  SQLiteDataFile.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/21/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "SQLiteDataFile.hh"
#include "SQLiteKeyStore.hh"
#include "SQLite_Internal.hh"
#include "Record.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "SharedKeys.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <mutex>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"
}

#if __APPLE__
#include <TargetConditionals.h>
#elif defined(_MSC_VER)
#include <arc4random.h>
#endif

using namespace std;

namespace litecore {


    LogDomain SQL("SQL");

    void LogStatement(const SQLite::Statement &st) {
        LogTo(SQL, "... %s", st.getQuery().c_str());
    }


    UsingStatement::UsingStatement(SQLite::Statement &stmt) noexcept
    :_stmt(stmt)
    {
        LogStatement(stmt);
    }


    UsingStatement::~UsingStatement() {
        try {
            _stmt.reset();
        } catch (...) { }
    }


    SQLiteDataFile::Factory& SQLiteDataFile::factory() {
        static SQLiteDataFile::Factory s;
        return s;
    }


    bool SQLiteDataFile::Factory::encryptionEnabled(EncryptionAlgorithm alg) {
        static int sEncryptionEnabled = -1;
        static once_flag once;
        call_once(once, []() {
            // Check whether encryption is available:
            if (sqlite3_compileoption_used("SQLITE_HAS_CODEC") == 0) {
                sEncryptionEnabled = false;
            } else {
                // Determine whether we're using SQLCipher or the SQLite Encryption Extension,
                // by calling a SQLCipher-specific pragma that returns a number:
                SQLite::Database sqlDb(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
                SQLite::Statement s(sqlDb, "PRAGMA cipher_default_kdf_iter");
                LogStatement(s);
                sEncryptionEnabled = s.executeStep();
            }
        });
        return sEncryptionEnabled > 0 && (alg == kNoEncryption || alg == kAES256);
    }


    SQLiteDataFile* SQLiteDataFile::Factory::openFile(const FilePath &path, const Options *options) {
        return new SQLiteDataFile(path, options);
    }


    bool SQLiteDataFile::Factory::deleteFile(const FilePath &path, const Options*) {
        return path.del() | path.appendingToName("-shm").del() | path.appendingToName("-wal").del();
        // Note the non-short-circuiting 'or'!
    }


    SQLiteDataFile::SQLiteDataFile(const FilePath &path, const Options *options)
    :DataFile(path, options)
    {
        reopen();
    }


    SQLiteDataFile::~SQLiteDataFile() {
        close();
    }


    void SQLiteDataFile::reopen() {
        DataFile::reopen();
        int sqlFlags = options().writeable ? SQLite::OPEN_READWRITE : SQLite::OPEN_READONLY;
        if (options().create)
            sqlFlags |= SQLite::OPEN_CREATE;
        _sqlDb = make_unique<SQLite::Database>(filePath().path().c_str(), sqlFlags);

        if (!decrypt())
            error::_throw(error::UnsupportedEncryption);

        withFileLock([this]{
            // http://www.sqlite.org/pragma.html
            exec("PRAGMA mmap_size=50000000; "          // mmap improves performance
                 "PRAGMA journal_mode=WAL; "            // faster writes, better concurrency
                 "PRAGMA journal_size_limit=5000000; "  // trim WAL file to 5MB
                 "PRAGMA synchronous=normal; "          // faster commits
                 "CREATE TABLE IF NOT EXISTS "          // Table of metadata about KeyStores
                        "kvmeta (name TEXT PRIMARY KEY, lastSeq INTEGER DEFAULT 0) WITHOUT ROWID");
#if DEBUG
            if (arc4random() % 1)              // deliberately make unordered queries unpredictable
                _sqlDb->exec("PRAGMA reverse_unordered_selects=1");
#endif

            // Configure number of extra threads to be used by SQLite:
            int maxThreads = 0;
#if TARGET_OS_OSX
            maxThreads = 2;
            // TODO: Configure for other platforms
#endif
            sqlite3_limit(_sqlDb->getHandle(), SQLITE_LIMIT_WORKER_THREADS, maxThreads);

            // Create the default KeyStore's table:
            (void)defaultKeyStore();
        });
    }


    void SQLiteDataFile::registerFleeceFunctions() {
        if (!_registeredFleeceFunctions) {
            auto sqlite = _sqlDb->getHandle();
            RegisterFleeceFunctions    (sqlite, fleeceAccessor(), documentKeys());
            RegisterFleeceEachFunctions(sqlite, fleeceAccessor(), documentKeys());
            RegisterFTSRankFunction(sqlite);
            register_unicodesn_tokenizer(sqlite);
            _registeredFleeceFunctions = true;
        }
    }


    bool SQLiteDataFile::isOpen() const noexcept {
        return _sqlDb != nullptr;
    }


    void SQLiteDataFile::close() {
        DataFile::close(); // closes all the KeyStores
        _getLastSeqStmt.reset();
        _setLastSeqStmt.reset();
        _sqlDb.reset();
    }


    bool SQLiteDataFile::decrypt() {
        auto alg = options().encryptionAlgorithm;
        if (alg != kNoEncryption) {
            if (!factory().encryptionEnabled(alg))
                return false;

            // Set the encryption key in SQLite:
            slice key = options().encryptionKey;
            if(key.buf == nullptr || key.size != 32)
                error::_throw(error::InvalidParameter);
            exec(string("PRAGMA key = \"x'") + key.hexString() + "'\"");
        }

        // Verify that encryption key is correct (or db is unencrypted, if no key given):
        exec("SELECT count(*) FROM sqlite_master");
        return true;
    }


    void SQLiteDataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
        switch (alg) {
            case kNoEncryption:
                break;
            case kAES256:
                if(newKey.buf == nullptr || newKey.size != 32)
                    error::_throw(error::InvalidParameter);
                break;
            default:
                error::_throw(error::InvalidParameter);
        }

        if (!factory().encryptionEnabled(alg))
            error::_throw(error::UnsupportedEncryption);

        // Get the userVersion of the db:
        int userVersion = 0;
        {
            SQLite::Statement st(*_sqlDb, "PRAGMA user_version");
            LogStatement(st);
            if (st.executeStep())
                userVersion = st.getColumn(0);
        }

        // Make a path for a temporary database file:
        const FilePath &realPath = filePath();
        FilePath tempPath(realPath.dirName(), "_rekey_temp.sqlite3");
        factory().deleteFile(tempPath);

        // Create & attach a temporary database encrypted with the new key:
        {
            string sql = "ATTACH DATABASE ? AS rekeyed_db KEY ";
            if (alg == kNoEncryption)
                sql += "''";
            else
                sql += "\"x'" + newKey.hexString() + "'\"";
            SQLite::Statement attach(*_sqlDb, sql);
            attach.bind(1, tempPath);
            LogStatement(attach);
            attach.executeStep();
        }

        try {

            // Export the current database's contents to the new one:
            // <https://www.zetetic.net/sqlcipher/sqlcipher-api/#sqlcipher_export>
            {
                exec("SELECT sqlcipher_export('rekeyed_db')");

                stringstream sql;
                sql << "PRAGMA rekeyed_db.user_version = " << userVersion;
                exec(sql.str());
            }

            // Close the old database:
            close();

            // Replace it with the new one:
            try {
                factory().deleteFile(realPath);
            } catch (const error &e) {
                // ignore errors deleting old files
            }
            factory().moveFile(tempPath, realPath);

        } catch (const exception &x) {
            // Back out and rethrow:
            close();
            factory().deleteFile(tempPath);
            reopen();
            throw;
        }

        // Update encryption key:
        auto opts = options();
        opts.encryptionAlgorithm = alg;
        opts.encryptionKey = newKey;
        setOptions(opts);

        // Finally reopen:
        reopen();
    }


    KeyStore* SQLiteDataFile::newKeyStore(const string &name, KeyStore::Capabilities options) {
        return new SQLiteKeyStore(*this, name, options);
    }

    void SQLiteDataFile::deleteKeyStore(const string &name) {
        execWithLock(string("DROP TABLE IF EXISTS kv_") + name);
        execWithLock(string("DROP TABLE IF EXISTS kvold_") + name);
    }


    void SQLiteDataFile::_beginTransaction(Transaction*) {
        checkOpen();
        Assert(_transaction == nullptr);
        LogTo(SQL, "BEGIN");
        _transaction = make_unique<SQLite::Transaction>(*_sqlDb);
    }


    void SQLiteDataFile::_endTransaction(Transaction *t, bool commit) {
        // Notify key-stores so they can save state:
        forOpenKeyStores([commit](KeyStore &ks) {
            ((SQLiteKeyStore&)ks).transactionWillEnd(commit);
        });

        // Now commit:
        if (commit) {
            LogTo(SQL, "COMMIT");
            _transaction->commit();
        } else {
            LogTo(SQL, "ROLLBACK");
        }
        _transaction.reset(); // destruct SQLite::Transaction, which will rollback if not committed
    }


    int SQLiteDataFile::exec(const string &sql) {
        LogTo(SQL, "%s", sql.c_str());
        return _sqlDb->exec(sql);
    }


    int SQLiteDataFile::execWithLock(const string &sql) {
        checkOpen();
        int result;
        withFileLock([&]{
            result = exec(sql);
        });
        return result;
    }


    SQLite::Statement& SQLiteDataFile::compile(const unique_ptr<SQLite::Statement>& ref,
                                               const char *sql) const
    {
        checkOpen();
        if (ref == nullptr) {
            try {
                const_cast<unique_ptr<SQLite::Statement>&>(ref)
                                                    = make_unique<SQLite::Statement>(*_sqlDb, sql);
            } catch (const SQLite::Exception &x) {
                Warn("SQLite error compiling statement \"%s\": %s", sql, x.what());
                throw;
            }
        }
        return *ref.get();
    }


    bool SQLiteDataFile::tableExists(const string &name) const {
        checkOpen();
        SQLite::Statement st(*_sqlDb, string("SELECT * FROM sqlite_master"
                                             " WHERE type='table' AND name=?"));
        st.bind(1, name);
        LogStatement(st);
        bool exists = st.executeStep();
        st.reset();
        return exists;
    }

    
    sequence SQLiteDataFile::lastSequence(const string& keyStoreName) const {
        sequence seq = 0;
        compile(_getLastSeqStmt, "SELECT lastSeq FROM kvmeta WHERE name=?");
        UsingStatement u(_getLastSeqStmt);
        _getLastSeqStmt->bindNoCopy(1, keyStoreName);
        if (_getLastSeqStmt->executeStep())
            seq = (int64_t)_getLastSeqStmt->getColumn(0);
        return seq;
    }

    void SQLiteDataFile::setLastSequence(SQLiteKeyStore &store, sequence seq) {
        compile(_setLastSeqStmt,
                "INSERT OR REPLACE INTO kvmeta (name, lastSeq) VALUES (?, ?)");
        UsingStatement u(_setLastSeqStmt);
        _setLastSeqStmt->bindNoCopy(1, store.name());
        _setLastSeqStmt->bind(2, (long long)seq);
        _setLastSeqStmt->exec();
    }


    void SQLiteDataFile::deleteDataFile() {
        close();
        factory().deleteFile(filePath());
    }


    void SQLiteDataFile::compact() {
        checkOpen();
        beganCompacting();
        {
            Transaction t(this);
            for (auto& name : allKeyStoreNames()) {
                exec(string("DELETE FROM kv_")+name+" WHERE deleted=1");
                if (options().keyStores.getByOffset) {
                    exec(string("DELETE FROM kvold_")+name);
                }
            }
            updatePurgeCount(t);
            t.commit();
        }
        exec(string("VACUUM"));     // Vacuum can't be called in a transaction
        finishedCompacting();
    }

}
