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

#if __APPLE__
#include <TargetConditionals.h>
#endif

using namespace std;

namespace litecore {


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
            _sqlDb->exec("PRAGMA mmap_size=50000000");      // mmap improves performance
            _sqlDb->exec("PRAGMA journal_mode=WAL");        // faster writes, better concurrency
            _sqlDb->exec("PRAGMA journal_size_limit=5000000"); // trim WAL file to 5MB
            _sqlDb->exec("PRAGMA synchronous=normal");      // faster commits
#if DEBUG
            if (arc4random() % 1)              // deliberately make unordered queries unpredictable
                _sqlDb->exec("PRAGMA reverse_unordered_selects=1");
#endif

            // Configure number of extra threads to be used by SQLite:
            int maxThreads = 0;
#if TARGET_OS_OSX
            maxThreads = 2;
#endif
            // TODO: Add tests for other platforms
            sqlite3_limit(_sqlDb->getHandle(), SQLITE_LIMIT_WORKER_THREADS, maxThreads);

            // Table containing metadata about KeyStores:
            _sqlDb->exec("CREATE TABLE IF NOT EXISTS kvmeta (name TEXT PRIMARY KEY,"
                         " lastSeq INTEGER DEFAULT 0) WITHOUT ROWID");

            // Create the default KeyStore's table:
            (void)defaultKeyStore();
        });
    }


    void SQLiteDataFile::registerFleeceFunctions() {
        if (!_registeredFleeceFunctions) {
            RegisterFleeceFunctions    (_sqlDb->getHandle(), fleeceAccessor(), documentKeys());
            RegisterFleeceEachFunctions(_sqlDb->getHandle(), fleeceAccessor(), documentKeys());
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
            _sqlDb->exec(string("PRAGMA key = \"x'") + key.hexString() + "'\"");
        }

        // Verify that encryption key is correct (or db is unencrypted, if no key given):
        _sqlDb->exec("SELECT count(*) FROM sqlite_master");
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
            attach.executeStep();
        }

        try {

            // Export the current database's contents to the new one:
            // <https://www.zetetic.net/sqlcipher/sqlcipher-api/#sqlcipher_export>
            {
                _sqlDb->exec("SELECT sqlcipher_export('rekeyed_db')");

                stringstream sql;
                sql << "PRAGMA rekeyed_db.user_version = " << userVersion;
                _sqlDb->exec(sql.str());
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
        exec(string("DROP TABLE IF EXISTS kv_") + name);
        exec(string("DROP TABLE IF EXISTS kvold_") + name);
    }


    void SQLiteDataFile::_beginTransaction(Transaction*) {
        checkOpen();
        Assert(_transaction == nullptr);
        _transaction = make_unique<SQLite::Transaction>(*_sqlDb);
    }


    void SQLiteDataFile::_endTransaction(Transaction *t, bool commit) {
        // Notify key-stores so they can save state:
        forOpenKeyStores([commit](KeyStore &ks) {
            ((SQLiteKeyStore&)ks).transactionWillEnd(commit);
        });

        // Now commit:
        if (commit)
            _transaction->commit();
        _transaction.reset(); // destruct SQLite::Transaction, which will rollback if not committed
    }


    int SQLiteDataFile::exec(const string &sql) {
        checkOpen();
        int result;
        withFileLock([&]{ result = _sqlDb->exec(sql); });
        return result;
    }


    SQLite::Statement& SQLiteDataFile::compile(const unique_ptr<SQLite::Statement>& ref,
                                               const char *sql) const
    {
        checkOpen();
        if (ref == nullptr)
            const_cast<unique_ptr<SQLite::Statement>&>(ref) = make_unique<SQLite::Statement>(*_sqlDb, sql);
        return *ref.get();
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
        _setLastSeqStmt->bind(2, (int64_t)seq);
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
                _sqlDb->exec(string("DELETE FROM kv_")+name+" WHERE deleted=1");
                if (options().keyStores.getByOffset) {
                    _sqlDb->exec(string("DELETE FROM kvold_")+name);
                }
            }
            updatePurgeCount(t);
            t.commit();
        }
        _sqlDb->exec(string("VACUUM"));     // Vacuum can't be called in a transaction
        finishedCompacting();
    }

}
