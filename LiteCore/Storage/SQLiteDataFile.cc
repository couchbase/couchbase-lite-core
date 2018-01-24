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
#include "UnicodeCollator.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "SharedKeys.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "PlatformCompat.hh"
#include "FleeceCpp.hh"
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <mutex>
#include <thread>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"
}

#if __APPLE__
#include <TargetConditionals.h>
#else
#include <arc4random.h>
#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#include "SQLiteTempDirectory.h"
#endif
#endif

using namespace std;

namespace litecore {

    static const int64_t MB = 1024 * 1024;

    // Min/max user_version of db files I can read
    static const int kMinUserVersion = 201;
    static const int kMaxUserVersion = 299;

    // SQLite page size
    static const int64_t kPageSize = 4096;

    // SQLite cache size (per connection)
    static const size_t kCacheSize = 10 * MB;

    // Maximum size WAL journal will be left at after a commit
    static const int64_t kJournalSize = 5 * MB;

    // Amount of file to memory-map
#if TARGET_OS_OSX || TARGET_OS_SIMULATOR
    static const int kMMapSize =  -1;    // Avoid possible file corruption hazard on macOS
#else
    static const int kMMapSize = 50 * MB;
#endif

    // If this fraction of the database is composed of free pages, vacuum it
    static const float kVacuumFractionThreshold = 0.25;
    // If the database has many bytes of free space, vacuum it
    static const int64_t kVacuumSizeThreshold = 50 * MB;

    // Database busy timeout; generally not needed since we have other arbitration that keeps
    // multiple threads from trying to start transactions at once, but another process might
    // open the database and grab the write lock.
    static const unsigned kBusyTimeoutSecs = 10;

    // How long deleteDataFile() should wait for other threads to close their connections
    static const unsigned kOtherDBCloseTimeoutSecs = 3;


    LogDomain SQL("SQL");

    void LogStatement(const SQLite::Statement &st) {
        LogVerbose(SQL, "... %s", st.getQuery().c_str());
    }

    static void sqlite3_log_callback(void *pArg, int errCode, const char *msg) {
        if (errCode == SQLITE_NOTICE_RECOVER_WAL)
            return;     // harmless "recovered __ frames from WAL file" message
        int baseCode = errCode & 0xFF;
        if (baseCode == SQLITE_SCHEMA)
            return;     // ignore harmless "statement aborts ... database schema has changed" warning
        if (baseCode == SQLITE_NOTICE || baseCode == SQLITE_READONLY) {
            Log("SQLite message: %s", msg);
        } else {
            Warn("SQLite error (code %d): %s", errCode, msg);
        }
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


    SQLiteDataFile::Factory::Factory() {
        // One-time initialization at startup:
        Assert(sqlite3_libversion_number() >= 300900, "LiteCore requires SQLite 3.9+");
        sqlite3_config(SQLITE_CONFIG_LOG, sqlite3_log_callback, NULL);
#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        setSqliteTempDirectory();
#endif
    }


    bool SQLiteDataFile::Factory::encryptionEnabled(EncryptionAlgorithm alg) {
#ifdef COUCHBASE_ENTERPRISE
        static int sEncryptionEnabled = -1;
        static once_flag once;
        call_once(once, []() {
            // Check whether encryption is available:
            sEncryptionEnabled = sqlite3_compileoption_used("SQLITE_HAS_CODEC");
        });
        return sEncryptionEnabled > 0 && (alg == kNoEncryption || alg == kAES256);
#else
        return false;
#endif

    }


    SQLiteDataFile* SQLiteDataFile::Factory::openFile(const FilePath &path, const Options *options) {
        return new SQLiteDataFile(path, options);
    }


    bool SQLiteDataFile::Factory::deleteFile(const FilePath &path, const Options*) {
        auto count = (unsigned) openCount(path);
        if (count > 0)
            error::_throw(error::Busy, "Still %u open connection(s) to %s",
                          count,
path.path().c_str());
        return path.del() | path.appendingToName("-shm").del() | path.appendingToName("-wal").del();
        // Note the non-short-circuiting 'or'! All 3 paths will be deleted.
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
        _sqlDb = make_unique<SQLite::Database>(filePath().path().c_str(),
                                               sqlFlags,
                                               kBusyTimeoutSecs * 1000);

        if (!decrypt())
            error::_throw(error::UnsupportedEncryption);

        if (sqlite3_libversion_number() < 003012) {
            // Prior to 3.12, the default page size was 1024, which is less than optimal.
            // Note that setting the page size has to be done before any other command that touches
            // the database file.
            // The cache_size value is negative to tell SQLite it's in KB (hence the /1024.)
            _exec(format("PRAGMA page_size=%lld", (long long)kPageSize));
        }

        withFileLock([this]{
            // http://www.sqlite.org/pragma.html
            int userVersion = _sqlDb->execAndGet("PRAGMA user_version");
            if (userVersion == 0) {
                // Configure persistent db settings, and create the schema:
                _exec("PRAGMA journal_mode=WAL; "        // faster writes, better concurrency
                     "PRAGMA auto_vacuum=incremental; " // incremental vacuum mode
                     "BEGIN; "
                     "CREATE TABLE IF NOT EXISTS "      // Table of metadata about KeyStores
                     "  kvmeta (name TEXT PRIMARY KEY, lastSeq INTEGER DEFAULT 0) WITHOUT ROWID; "
                     );
                // Create the default KeyStore's table:
                (void)defaultKeyStore();
                _exec("PRAGMA user_version=201; "
                      "END;");
            } else if (userVersion < kMinUserVersion) {
                error::_throw(error::DatabaseTooOld);
            } else if (userVersion > kMaxUserVersion) {
                error::_throw(error::DatabaseTooNew);
            }
        });

        _exec(format("PRAGMA cache_size=%d; "            // Memory cache
                     "PRAGMA mmap_size=%d; "             // Memory-mapped reads
                     "PRAGMA synchronous=normal; "       // Speeds up commits
                     "PRAGMA journal_size_limit=%lld; "  // Limit WAL disk usage
                     "PRAGMA case_sensitive_like=true",  // Case sensitive LIKE, for N1QL compat
                     -(int)kCacheSize/1024, kMMapSize, (long long)kJournalSize));

#if DEBUG
        // Deliberately make unordered queries unpredictable, to expose any LiteCore code that
        // unintentionally relies on ordering:
        if (arc4random() % 1)
            _sqlDb->exec("PRAGMA reverse_unordered_selects=1");
#endif

        // Configure number of extra threads to be used by SQLite:
        int maxThreads = 0;
#if TARGET_OS_OSX
        maxThreads = 2;
        // TODO: Configure for other platforms
#endif
        auto sqlite = _sqlDb->getHandle();
        if (maxThreads > 0)
            sqlite3_limit(sqlite, SQLITE_LIMIT_WORKER_THREADS, maxThreads);

        // Register collators, custom functions, and the FTS tokenizer:
        RegisterSQLiteUnicodeCollations(sqlite, _collationContexts);
        RegisterSQLiteFunctions(sqlite, fleeceAccessor(), documentKeys());
        int rc = register_unicodesn_tokenizer(sqlite);
        if (rc != SQLITE_OK)
            Warn("Unable to register FTS tokenizer: SQLite err %d", rc);
    }


    bool SQLiteDataFile::isOpen() const noexcept {
        return _sqlDb != nullptr;
    }


    void SQLiteDataFile::close() {
        DataFile::close(); // closes all the KeyStores
        _getLastSeqStmt.reset();
        _setLastSeqStmt.reset();
        if (_sqlDb) {
            optimizeAndVacuum();
            _sqlDb.reset();
        }
        _collationContexts.clear();
    }


    bool SQLiteDataFile::decrypt() {
        auto alg = options().encryptionAlgorithm;
        if (alg != kNoEncryption) {
#ifdef COUCHBASE_ENTERPRISE
            if (!factory().encryptionEnabled(alg))
                return false;

            // Set the encryption key in SQLite:
            slice key = options().encryptionKey;
            if(key.buf == nullptr || key.size != 32)
                error::_throw(error::InvalidParameter);
            sqlite3_key_v2(_sqlDb->getHandle(), nullptr, key.buf, key.size);
#else
            error::_throw(error::UnsupportedOperation);
#endif
        }

        // Verify that encryption key is correct (or db is unencrypted, if no key given):
        _exec("SELECT count(*) FROM sqlite_master");
        return true;
    }


    void SQLiteDataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
#ifdef COUCHBASE_ENTERPRISE
        bool currentlyEncrypted = (options().encryptionAlgorithm != kNoEncryption);
        switch (alg) {
            case kNoEncryption:
                if (!currentlyEncrypted)
                    return;
                LogTo(DBLog, "Decrypting DataFile");
                break;
            case kAES256:
                if (currentlyEncrypted) {
                    LogTo(DBLog, "Changing DataFile encryption key");
                }
                else {
                    LogTo(DBLog, "Encrypting DataFile");
                }

                if(newKey.buf == nullptr || newKey.size != 32)
                    error::_throw(error::InvalidParameter);
                break;
            default:
                error::_throw(error::InvalidParameter);
        }

        if (!factory().encryptionEnabled(alg))
            error::_throw(error::UnsupportedEncryption);

        int rekeyResult = 0;
        if(alg == kNoEncryption) {
            rekeyResult = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, nullptr, 0);
        } else {
            rekeyResult = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, newKey.buf, newKey.size);
        }
        
        if(rekeyResult != SQLITE_OK) {
            error::_throw(litecore::error::SQLite, rekeyResult);
        }

        // Update encryption key:
        auto opts = options();
        opts.encryptionAlgorithm = alg;
        opts.encryptionKey = newKey;
        setOptions(opts);

        // Finally reopen:
        reopen();
#else
        error::_throw(litecore::error::UnsupportedOperation);
#endif
    }


    KeyStore* SQLiteDataFile::newKeyStore(const string &name, KeyStore::Capabilities options) {
        return new SQLiteKeyStore(*this, name, options);
    }

#if ENABLE_DELETE_KEY_STORES
    void SQLiteDataFile::deleteKeyStore(const string &name) {
        execWithLock(string("DROP TABLE IF EXISTS kv_") + name);
    }
#endif

    void SQLiteDataFile::_beginTransaction(Transaction*) {
        checkOpen();
        _exec("BEGIN");
    }


    void SQLiteDataFile::_endTransaction(Transaction *t, bool commit) {
        // Notify key-stores so they can save state:
        forOpenKeyStores([commit](KeyStore &ks) {
            ((SQLiteKeyStore&)ks).transactionWillEnd(commit);
        });

        exec(commit ? "COMMIT" : "ROLLBACK");
    }


    void SQLiteDataFile::beginReadOnlyTransaction() {
        checkOpen();
        _exec("SAVEPOINT roTransaction");
    }

    void SQLiteDataFile::endReadOnlyTransaction() {
        _exec("RELEASE SAVEPOINT roTransaction");
    }


    int SQLiteDataFile::_exec(const string &sql, LogLevel logLevel) {
        if (_usuallyFalse(SQL.willLog(logLevel)))
            SQL.log(logLevel, "%s", sql.c_str());
        return _sqlDb->exec(sql);
    }

    int SQLiteDataFile::exec(const string &sql, LogLevel logLevel) {
        Assert(inTransaction());
        return _exec(sql, logLevel);
    }

    int SQLiteDataFile::execWithLock(const string &sql) {
        checkOpen();
        int result;
        withFileLock([&]{
            result = _exec(sql);
        });
        return result;
    }


    int64_t SQLiteDataFile::intQuery(const char *query) {
        SQLite::Statement st(*_sqlDb, query);
        LogStatement(st);
        return st.executeStep() ? st.getColumn(0) : 0;
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

    
    sequence_t SQLiteDataFile::lastSequence(const string& keyStoreName) const {
        sequence_t seq = 0;
        compile(_getLastSeqStmt, "SELECT lastSeq FROM kvmeta WHERE name=?");
        UsingStatement u(_getLastSeqStmt);
        _getLastSeqStmt->bindNoCopy(1, keyStoreName);
        if (_getLastSeqStmt->executeStep())
            seq = (int64_t)_getLastSeqStmt->getColumn(0);
        return seq;
    }

    void SQLiteDataFile::setLastSequence(SQLiteKeyStore &store, sequence_t seq) {
        compile(_setLastSeqStmt,
                "INSERT OR REPLACE INTO kvmeta (name, lastSeq) VALUES (?, ?)");
        UsingStatement u(_setLastSeqStmt);
        _setLastSeqStmt->bindNoCopy(1, store.name());
        _setLastSeqStmt->bind(2, (long long)seq);
        _setLastSeqStmt->exec();
    }


    void SQLiteDataFile::deleteDataFile() {
        // Wait for other connections to close -- in multithreaded setups there may be races where
        // another thread takes a bit longer to close its connection.
        fleece::Stopwatch st;
        while (factory().openCount(filePath()) > 1) {
            if (st.elapsed() > kOtherDBCloseTimeoutSecs)
                error::_throw(error::Busy);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        close();
        factory().deleteFile(filePath());
    }


    void SQLiteDataFile::optimizeAndVacuum() {
        // <https://sqlite.org/pragma.html#pragma_optimize>
        // <https://blogs.gnome.org/jnelson/2015/01/06/sqlite-vacuum-and-auto_vacuum/>
        try {
            int64_t pageCount = intQuery("PRAGMA page_count");
            int64_t freePages = intQuery("PRAGMA freelist_count");
            LogVerbose(DBLog, "Pre-close housekeeping: %lld of %lld pages free (%.0f%%)",
                       (long long)freePages, (long long)pageCount, (float)freePages / pageCount);

            _exec("PRAGMA optimize");

            if ((pageCount > 0 && (float)freePages / pageCount >= kVacuumFractionThreshold)
                    || (freePages * kPageSize >= kVacuumSizeThreshold)) {
                Log("Vacuuming database '%s'...", filePath().dirName().c_str());
                _exec("PRAGMA incremental_vacuum");
            }
        } catch (const SQLite::Exception &x) {
            Warn("Caught SQLite exception while vacuuming: %s", x.what());
        }
    }


    void SQLiteDataFile::compact() {
        checkOpen();
        optimizeAndVacuum();
    }


    alloc_slice SQLiteDataFile::rawQuery(const string &query) {
        SQLite::Statement stmt(*_sqlDb, query);
        int nCols = stmt.getColumnCount();
        fleeceapi::Encoder enc;
        enc.beginArray();
        while (stmt.executeStep()) {
            enc.beginArray();
            for (int i = 0; i < nCols; ++i) {
                SQLite::Column col = stmt.getColumn(i);
                switch (col.getType()) {
                    case SQLITE_NULL:
                        enc.writeNull(); break;
                    case SQLITE_INTEGER:
                        enc.writeInt(col.getInt64()); break;
                    case SQLITE_FLOAT:
                        enc.writeDouble(col.getDouble()); break;
                    case SQLITE_TEXT:
                        enc.writeString(col.getString()); break;
                    case SQLITE_BLOB:
                        enc.writeData(slice(col.getBlob(), col.getBytes())); break;
                }
            }
            enc.endArray();
        }
        enc.endArray();
        return enc.finish();
    }

}
