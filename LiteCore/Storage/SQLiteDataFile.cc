//
// SQLiteDataFile.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

/*
 * DataFile version history
 * 201: Initial Version
 * 301: Add index table for use with FTS
 * 302: Add purgeCnt entry to kvmeta
 */

#include "SQLiteDataFile.hh"
#include "SQLiteKeyStore.hh"
#include "SQLite_Internal.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "BothKeyStore.hh"
#include "carray.h"
#include "UnicodeCollator.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "SharedKeys.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "Extension.hh"
#include "Defer.hh"
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <mutex>
#include <regex>
#include <thread>
#include <cinttypes>
#ifdef _WIN32
#    include <Windows.h>
#endif

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"

#ifdef COUCHBASE_ENTERPRISE
// These definitions were removed from the public SQLite header
// in 3.32.0, and Xcode doesn't like to alter header search paths
// between configurations, so as a compromise just lay them out here:
SQLITE_API int sqlite3_key_v2(sqlite3*    db,            /* Database to be rekeyed */
                              const char* zDbName,       /* Name of the database */
                              const void* pKey, int nKey /* The key */
);

SQLITE_API int sqlite3_rekey_v2(sqlite3*    db,            /* Database to be rekeyed */
                                const char* zDbName,       /* Name of the database */
                                const void* pKey, int nKey /* The new key */
);
#endif
}

#if __APPLE__
#    include <TargetConditionals.h>
#endif

using namespace std;

namespace litecore {

    static const int64_t MB = 1024 * 1024;

    // SQLite page size
    static const int64_t kPageSize = 4096;

    // SQLite cache size (per connection)
    static const size_t kCacheSize = 10 * MB;

    // Maximum size WAL journal will be left at after a commit
    static const int64_t kJournalSize = 5 * MB;

    static map<string, int> kValidExtensionVersions = {
#ifdef COUCHBASE_ENTERPRISE
            {"CouchbaseLiteVectorSearch", 1}
#endif
    };

    // Amount of file to memory-map
    // https://www.sqlite.org/mmap.html#:~:text=To%20disable%20memory%2Dmapped%20I,any%20content%20beyond%20N%20bytes.
#if TARGET_OS_OSX || TARGET_OS_SIMULATOR
    static const int kMMapSize = 0;  // Avoid possible file corruption hazard on macOS
#else
    static const int kMMapSize = 50 * MB;
#endif

    // If this fraction of the database is composed of free pages, vacuum it on close
    static const float kVacuumFractionThreshold = 0.25;
    // If the database has many bytes of free space, vacuum it on close
    static const int64_t kVacuumSizeThreshold = 10 * MB;

    // Database busy timeout; generally not needed since we have other arbitration that keeps
    // multiple threads from trying to start transactions at once, but another process might
    // open the database and grab the write lock.
    static const unsigned kBusyTimeoutSecs = 10;

    // Prefix of the KeyStores for deleted documents
    static const string kDeletedKeyStorePrefix = "del_";

    // Directory where SQLite extensions are found (set by setExtensionPath() fn)
    static string sExtensionPath;

    LogDomain SQL("SQL", LogLevel::Warning);

    void LogStatement(const SQLite::Statement& st) { LogTo(SQL, "... %s", st.getQuery().c_str()); }

    static std::pair<bool, std::string> enhanceSQLiteErrorLog(int errCode, const char* msg) {
        std::regex  noTableRegx{"no such table: (\\S+) in "};
        std::cmatch match;
        const char* extra = nullptr;
        if ( std::regex_search(msg, match, noTableRegx) ) {
            if ( std::regex_search(match.suffix().str(), std::regex{"fl_unnested_value"})
                 && std::regex_match(match[1].str(), std::regex{"[0-9a-z]{40}"}) ) {
                extra = "This table is referenced by an array index, which may have been deleted.";
            } else if ( std::regex_match(match[1].str(), std::regex{"kv_\\..+::.+"}) ) {
                // example target: match[1].str() = "kv_.namedscope.names::by\\Street"
                // where "::" = KeyStore::kIndexSeparator
                extra = "This table is referenced by an FTS index, which may have been deleted.";
            }
        }
        std::string enhanced;
        if ( extra ) {
            enhanced = msg;
            enhanced += ". "s + extra;
        }
        return std::make_pair(extra, enhanced);
    }

    static void sqlite3_log_callback(void* /*pArg*/, int errCode, const char* msg) {
        switch ( errCode & 0xFF ) {
            case SQLITE_OK:
            case SQLITE_NOTICE:
            case SQLITE_READONLY:
            case SQLITE_CONSTRAINT:
                if ( errCode == SQLITE_NOTICE_RECOVER_WAL )
                    break;  // harmless "recovered __ frames from WAL file" message
                LogTo(DBLog, "SQLite message: %s", msg);
                break;
            case SQLITE_SCHEMA:
                break;  // ignore harmless "statement aborts ... database schema has changed" warning
            case SQLITE_WARNING:
                if ( strncmp(msg, "file unlinked while open:", 25) == 0 )
                    break;  // ignore warning closing zombie db that's been deleted (#381)
                LogWarn(DBLog, "SQLite warning: %s", msg);
                break;
            default:
                {
                    auto [enhanced, enhancedMsg] = enhanceSQLiteErrorLog(errCode, msg);
                    if ( enhanced ) msg = enhancedMsg.c_str();
                    LogError(DBLog, "SQLite error (code %d): %s", errCode, msg);
                }
                break;
        }
    }

    UsingStatement::UsingStatement(SQLite::Statement& stmt) noexcept : _stmt(stmt) { LogStatement(stmt); }

    UsingStatement::~UsingStatement() noexcept {
        try {
            _stmt.reset();
            _stmt.clearBindings();
        } catch ( ... ) {}
    }

    slice getColumnAsSlice(SQLite::Statement& stmt, int colIndex) {
        auto col = stmt.getColumn(colIndex);
        auto buf = col.getBlob();
        return {buf, static_cast<size_t>(col.getBytes())};
    }

    SQLiteDataFile::Factory& SQLiteDataFile::sqliteFactory() {
        static SQLiteDataFile::Factory s;
        return s;
    }

    SQLiteDataFile::Factory::Factory() {
        // One-time initialization at startup:
        SQLite::Exception::logger = [](const SQLite::Exception& x) {
            LogError(SQL, "%s (%d/%d)", x.what(), x.getErrorCode(), x.getExtendedErrorCode());
        };
        Assert(sqlite3_libversion_number() >= 300900, "LiteCore requires SQLite 3.9+");
        sqlite3_config(SQLITE_CONFIG_LOG, sqlite3_log_callback, NULL);
    }

    bool SQLiteDataFile::Factory::encryptionEnabled(EncryptionAlgorithm alg) {
#ifdef COUCHBASE_ENTERPRISE
        return (alg == kNoEncryption || alg == kAES256);
#else
        return (alg == kNoEncryption);
#endif
    }

    SQLiteDataFile* SQLiteDataFile::Factory::openFile(const FilePath& path, DataFile::Delegate* delegate,
                                                      const Options* options) {
        return new SQLiteDataFile(path, delegate, options);
    }

    bool SQLiteDataFile::Factory::_deleteFile(const FilePath& path, const Options*) {
        LogTo(DBLog, "Deleting database file %s (with -wal and -shm)", path.path().c_str());
        bool ok = (path.del() + path.appendingToName("-shm").del() + path.appendingToName("-wal").del()) != 0;
        // Note the use of '+' instead of '||'! All 3 paths will be deleted.
        LogDebug(DBLog, "...finished deleting database file %s (with -wal and -shm)", path.path().c_str());
        return ok;
    }

    void SQLiteDataFile::setExtensionPath(string path) { sExtensionPath = std::move(path); }

    void SQLiteDataFile::enableExtension(const string& name, string path) {
        auto extensionEntry = kValidExtensionVersions.find(name);
        if ( extensionEntry == kValidExtensionVersions.end() ) {
            error::_throw(error::LiteCoreError::InvalidParameter, "'%s' is not a known extension", name.c_str());
        }

        // NOTE: This logic will need to be changed later if we have more than one extension
        // and they reside in different directories
        if ( !sExtensionPath.empty() && sExtensionPath != path ) {
            WarnError("Extension path previously set to '%s' but being reset to '%s'.  This is not advisable!",
                      sExtensionPath.c_str(), path.c_str());
        }

        sExtensionPath = std::move(path);

#if defined(__ANDROID__)
        string pluginPath = sExtensionPath + FilePath::kSeparator + "lib" + name;
#else
        string pluginPath = sExtensionPath + FilePath::kSeparator + name;
#endif

        if ( !extension::check_extension_version(pluginPath, extensionEntry->second) ) {
            sExtensionPath.clear();
            error::_throw(error::UnsupportedOperation,
                          "Extension '%s' is not found or not compatible with this version of Couchbase Lite",
                          name.c_str());
        }
    }

    SQLiteDataFile::SQLiteDataFile(const FilePath& path, DataFile::Delegate* delegate, const Options* options)
        : DataFile(path, delegate, options) {
        reopen();
    }

    SQLiteDataFile::~SQLiteDataFile() { close(); }

    // A lot of this logic can be turned into a reusable function later if needed
    // for more extensions.
    static void LoadVectorSearchExtension(sqlite3* sqlite) {
#ifdef COUCHBASE_ENTERPRISE
#    if defined(__ANDROID__)
        static const char* extensionName = "libCouchbaseLiteVectorSearch";
#    else
        static const char* extensionName = "CouchbaseLiteVectorSearch";
#    endif

        if ( sExtensionPath.empty() ) return;

        // First enable extension loading (for security reasons it's off by default):
        int rc = sqlite3_db_config(sqlite, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL);
        if ( rc != SQLITE_OK ) {
            LogToAt(DBLog, Error, "Unable to enable SQLite extension loading: err %d", rc);
            error::_throw(error::UnexpectedError, "Unable to enable SQLite extension loading: err %d", rc);
        }

        DEFER {
            // Disable extension-loading again, for safety's sake.
            sqlite3_db_config(sqlite, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL);
        };

        string pluginPath = sExtensionPath + FilePath::kSeparator + extensionName;

#    if defined(_WIN32) && defined(_M_X64)
        // Flimsy hack to get around the fact that we need to load this dep from a non-standard
        // location, and SQLite only uses the basic LoadLibraryA
        string  windowsDependentPath = sExtensionPath + FilePath::kSeparator + "libomp140.x86_64.dll";
        HMODULE dep                  = LoadLibraryA(windowsDependentPath.c_str());
        if ( !dep ) { error::_throw(error::CantOpenFile, "Unable to load libomp140.x86_64.dll..."); }
#    endif

        char* message = nullptr;
        rc            = sqlite3_load_extension(sqlite, pluginPath.c_str(), nullptr, &message);
        if ( rc != SQLITE_OK ) {
            LogToAt(DBLog, Error, "Unable to load '%s' extension: %s (%d)", extensionName, message, rc);
            sqlite3_free(message);
            error::_throw(error::CantOpenFile, "Unable to load '%s' extension: %s (%d)", extensionName, message, rc);
        }

#endif
    }

    void SQLiteDataFile::reopen() {
        DataFile::reopen();
        reopenSQLiteHandle();
        decrypt();

        withFileLock([this] {
            // http://www.sqlite.org/pragma.html
            _schemaVersion = SchemaVersion((int)_sqlDb->execAndGet("PRAGMA user_version"));
            if ( _schemaVersion == SchemaVersion::None ) {
                // Configure persistent db settings, and create the schema.
                // `auto_vacuum` has to be enabled ASAP, before anything's written to the db!
                // (even setting `auto_vacuum` writes to the db, it turns out! See CBSE-7971.)
                _exec(stringprintf(
                        "PRAGMA auto_vacuum=incremental; "
                        "PRAGMA journal_mode=WAL; "
                        "BEGIN; "
                        "CREATE TABLE IF NOT EXISTS "  // Table of metadata about KeyStores
                        "  kvmeta (name TEXT PRIMARY KEY, lastSeq INTEGER DEFAULT 0, purgeCnt INTEGER DEFAULT 0) "
                        "WITHOUT ROWID; "
                        "PRAGMA user_version=%d; "
                        "END;",
                        (int)SchemaVersion::Current));
                Assert(intQuery("PRAGMA auto_vacuum") == 2, "Incremental vacuum was not enabled!");
                _schemaVersion = SchemaVersion::Current;
                // Create the default KeyStore's table:
                (void)defaultKeyStore();
            } else if ( _schemaVersion < SchemaVersion::MinReadable ) {
                error::_throw(error::DatabaseTooOld);
            } else if ( _schemaVersion > SchemaVersion::MaxReadable ) {
                error::_throw(error::DatabaseTooNew);
            }

            const Options& options = getOptions();

            _exec(stringprintf(
                    "PRAGMA cache_size=%d; "             // Memory cache
                    "PRAGMA mmap_size=%d; "              // Memory-mapped reads
                    "PRAGMA synchronous=%s; "            // Speeds up commits
                    "PRAGMA journal_size_limit=%lld; "   // Limit WAL disk usage
                    "PRAGMA case_sensitive_like=true; "  // Case sensitive LIKE, for N1QL compat
                    "PRAGMA fullfsync=ON",  // Attempt to mitigate damage due to sudden loss of power (iOS / macOS)
                    -(int)kCacheSize / 1024, options.mmapDisabled ? 0 : kMMapSize,
                    options.diskSyncFull ? "full" : "normal", (long long)kJournalSize));

            (void)upgradeSchema(SchemaVersion::WithPurgeCount, "Adding purgeCnt column", [&] {
                // Schema upgrade: Add the `purgeCnt` column to the kvmeta table.
                // We can postpone this schema change if the db is read-only, since the purge count
                // is only used to mark changes to the database, and we won't be changing it.
                _exec("ALTER TABLE kvmeta ADD COLUMN purgeCnt INTEGER DEFAULT 0;");
            });

            if ( !upgradeSchema(SchemaVersion::WithNewDocs, "Adding `extra` column", [&] {
                     // Add the 'extra' column to every KeyStore:
                     for ( string& name : allKeyStoreNames() ) {
                         if ( name.find("::") == string::npos ) {
                             // CBL-1741: Only update data tables, not FTS index tables
                             _exec("ALTER TABLE \"kv_" + name + "\" ADD COLUMN extra BLOB;");
                         }
                     }
                 }) ) {
                error::_throw(error::CantUpgradeDatabase);
            }

            (void)upgradeSchema(SchemaVersion::WithIndexesLastSeq, "Adding indexes.lastSeq column", [&] {
                string sql;
                if ( getSchema("indexes", "table", "indexes", sql) ) {
                    // Check if the table needs to be updated to add the 'lastSeq' column: (v3.2)
                    if ( sql.find("lastSeq") == string::npos ) { _exec("ALTER TABLE indexes ADD COLUMN lastSeq TEXT"); }
                }
            });

            (void)upgradeSchema(SchemaVersion::WithExpirationColumn, "Adding `expiration` column", [&] {
                // Add the 'expiration' column to every KeyStore:
                for ( string& name : allKeyStoreNames() ) {
                    // Only update data tables, not FTS index tables
                    if ( name.find("::") == string::npos ) {
                        string tableName = SQLiteKeyStore::tableName(name);
                        string sql;
                        // We need to check for existence of the expiration column first.
                        // Do not add it if it already exists in the table.
                        if ( getSchema(tableName, "table", tableName, sql) && sql.find("expiration") != string::npos )
                            continue;
                        // Only update data tables, not FTS index tables
                        _exec(stringprintf(
                                "ALTER TABLE \"%s\" ADD COLUMN expiration INTEGER; "
                                "CREATE INDEX \"%s_expiration\" ON \"%s\" (expiration) WHERE expiration not null",
                                tableName.c_str(), tableName.c_str(), tableName.c_str()));
                    }
                }
            });

            (void)upgradeSchema(SchemaVersion::WithIndexesWhereColumn, "Adding indexes.whereClause column", [&] {
                string sql;
                if ( getSchema("indexes", "table", "indexes", sql) ) {
                    // Check if the table needs to be updated to add the 'lastSeq' column: (v3.2)
                    if ( sql.find("whereClause") == string::npos ) {
                        _exec("ALTER TABLE indexes ADD COLUMN whereClause TEXT");
                    }
                }
            });
        });

        // Configure number of extra threads to be used by SQLite:
        auto sqlite = _sqlDb->getHandle();
        if ( thread::hardware_concurrency() > 2 ) sqlite3_limit(sqlite, SQLITE_LIMIT_WORKER_THREADS, 2);

        // Register collators, custom functions, the FTS tokenizer, and the `carray` extension:
        RegisterSQLiteUnicodeCollations(sqlite, _collationContexts);
        RegisterSQLiteFunctions(sqlite, {delegate(), documentKeys()});
        int rc = register_unicodesn_tokenizer(sqlite);
        if ( rc != SQLITE_OK ) warn("Unable to register FTS tokenizer: SQLite err %d", rc);
        char* errMsg = nullptr;
        rc           = sqlite3_carray_init(sqlite, &errMsg, nullptr);
        if ( rc != SQLITE_OK ) throw SQLite::Exception(errMsg, rc);

        // Load vector search extension if present:
        LoadVectorSearchExtension(sqlite);

        withFileLock([this] {
            if ( !upgradeSchema(SchemaVersion::WithDeletedTable, "Migrating deleted docs to `del_` tables", [&] {
                     // Migrate deleted docs to separate table:
                     _schemaVersion = SchemaVersion::WithDeletedTable;  // enable creating _del keystores
                     for ( const string& keyStoreName : allKeyStoreNames() ) {
                         if ( keyStoreNameIsCollection(keyStoreName) ) {
                             Assert(!hasPrefix(keyStoreName, kDeletedKeyStorePrefix));
                             (void)getKeyStore(keyStoreName);  // creates the `_del` keystore

                             // CBL-4377 :
                             // Do not move the deleted docs from the default collection to the deleted table
                             // as moving deleted doc operation could take several seconds or mins depending on
                             // the database and platform. This means that the deleted docs of the default collection
                             // could exists in both live an deleted keystore's table.
                             //
                             // Note: As we don't have collection support prior 3.1, this change only affects
                             // the default collection.
                             if ( keyStoreName != kDefaultKeyStoreName ) {
                                 _exec(stringprintf("INSERT INTO \"kv_%s%s\" "
                                                    "SELECT * FROM \"kv_%s\" WHERE (flags&1)!=0; "
                                                    "DELETE FROM \"kv_%s\" WHERE (flags&1)!=0;",
                                                    kDeletedKeyStorePrefix.c_str(), keyStoreName.c_str(),
                                                    keyStoreName.c_str(), keyStoreName.c_str()));
                             }
                         }
                     }
                 }) ) {
                error::_throw(error::CantUpgradeDatabase);
            }
        });
        // Enable some security features:
        sqlite3_db_config(sqlite, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    }

    bool SQLiteDataFile::upgradeSchema(SchemaVersion minVersion, const char* what, function_ref<void()> upgrade) {
        auto logUpgrade = [&](const char* msg) {
            logInfo("SCHEMA UPGRADE (%d-%d) %-s", (int)_schemaVersion, (int)minVersion, msg);
        };

        if ( _schemaVersion < minVersion ) {
            if ( !options().writeable ) {
                logUpgrade("skipped; cannot upgrade read-only connection");
                return false;
            }
            if ( !options().upgradeable ) {
                logUpgrade("blocked: opening with 'NoUpgrade' flag");
                error::_throw(error::CantUpgradeDatabase);
            }

            logUpgrade(what);
            bool inTransaction = false;
            try {
                _exec("BEGIN");
                inTransaction = true;
                upgrade();
                _exec(stringprintf("PRAGMA user_version=%d; END", (int)minVersion));
            } catch ( const SQLite::Exception& x ) {
                // Recover if the db file itself is read-only (but not opened with writeable=false)
                if ( x.getErrorCode() == SQLITE_READONLY ) {
                    logUpgrade("skipped; cannot upgrade read-only file");
                    if ( inTransaction ) _exec("ABORT");
                    auto opts      = options();
                    opts.writeable = false;
                    setOptions(opts);
                    return false;
                } else {
                    throw;
                }
            }
            _schemaVersion = minVersion;
        }
        return true;
    }

    void SQLiteDataFile::reopenSQLiteHandle() {
        // We are about to replace the sqlite3 handle, so the compiled statements
        // need to be cleared
        _getLastSeqStmt.reset();
        _setLastSeqStmt.reset();
        _getPurgeCntStmt.reset();
        _setPurgeCntStmt.reset();

        int sqlFlags = options().writeable ? SQLite::OPEN_READWRITE : SQLite::OPEN_READONLY;
        if ( options().create ) sqlFlags |= SQLite::OPEN_CREATE;
        _sqlDb = make_unique<SQLite::Database>(filePath().path().c_str(), sqlFlags, kBusyTimeoutSecs * 1000);
    }

    void SQLiteDataFile::ensureSchemaVersionAtLeast(SchemaVersion version) {
        if ( _schemaVersion < version ) {
            const auto versionSql = "PRAGMA user_version=" + to_string(int(version));
            _exec(versionSql);
            _schemaVersion = version;
        }
    }

    bool SQLiteDataFile::isOpen() const noexcept { return _sqlDb != nullptr; }

    // Called by DataFile::close (the public method)
    void SQLiteDataFile::_close(bool forDelete) {
        _getLastSeqStmt.reset();
        _setLastSeqStmt.reset();
        _getPurgeCntStmt.reset();
        _setPurgeCntStmt.reset();
        if ( _sqlDb ) {
            if ( options().writeable && !options().noHousekeeping ) {
                withFileLock([this]() {
                    optimize();
                    vacuum(false);
                });
            }
            // Close the SQLite database:
            if ( !_sqlDb->closeUnlessStatementsOpen() ) {
                // There are still SQLite statements (queries) open, probably in QueryEnumerators
                // that haven't been deleted yet -- this can happen if the client code has garbage-
                // collected objects owning those enumerators, which won't release them until their
                // finalizers run. (Couchbase Lite Java has this issue.)
                // We'll log info about the statements so this situation can be detected from logs.
                _sqlDb->withOpenStatements([this, forDelete](const char* sql, bool busy) {
                    _log((forDelete ? LogLevel::Warning : LogLevel::Info),
                         "SQLite::Database %p close deferred due to %s sqlite_stmt: %s", _sqlDb.get(),
                         (busy ? "busy" : "open"), sql);
                });
                if ( forDelete ) error::_throw(error::Busy, "SQLite db has active statements, can't be deleted");
                // Also, tell SQLite not to checkpoint the WAL when it eventually closes the db
                // (after the last statement is freed), as that can have disastrous effects if the
                // db has since been deleted and re-created: see issue #381 for gory details.
                int noCheckpointResult =
                        sqlite3_db_config(_sqlDb->getHandle(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
                Assert(noCheckpointResult == SQLITE_OK, "Failed to set SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE");
            }
            // Finally, delete the SQLite::Database instance:
            _sqlDb.reset();
            logVerbose("Closed SQLite database");
        }
        _collationContexts.clear();
    }

    void SQLiteDataFile::decrypt() {
        auto alg = options().encryptionAlgorithm;
        if ( !factory().encryptionEnabled(alg) ) error::_throw(error::UnsupportedEncryption);
#ifdef COUCHBASE_ENTERPRISE
        // Set the encryption key in SQLite:
        slice key;
        if ( alg != kNoEncryption ) {
            key = options().encryptionKey;
            if ( key.buf == nullptr || key.size != kEncryptionKeySize[alg] ) error::_throw(error::InvalidParameter);
        }
        bool success = _decrypt(alg, key);
#    if __APPLE__
        if ( !success && alg == kAES256 ) {
            // If using AES256, retry with AES128 for backward compatibility with earlier versions:
            logInfo("Retrying decryption with AES128...");
            reopenSQLiteHandle();
            success = _decrypt(kAES128, slice(key.buf, kEncryptionKeySize[kAES128]));
            if ( success ) {
                logInfo("Success! Database is decrypted.");
                if ( options().writeable && options().upgradeable ) {
                    // Now rekey with the full AES256 key:
                    logInfo("Rekeying db to full AES256 encryption; this may take time...");
                    int rc = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, key.buf, (int)key.size);
                    if ( rc != SQLITE_OK ) {
                        logError("Rekeying to AES256 failed (err %d); continuing with existing db", rc);
                    }
                }
            }
        }
#    endif
        if ( !success ) error::_throw(error::NotADatabaseFile);
#endif
    }


#ifdef COUCHBASE_ENTERPRISE
    // Returns true on success, false if key is not valid; other errors thrown as exceptions.
    bool SQLiteDataFile::_decrypt(EncryptionAlgorithm alg, slice key) {
        static const char* kAlgorithmName[3] = {"no encryption", "AES256", "AES128"};
        // Calling sqlite3_key_v2 even with a null key (no encryption) reserves space in the db
        // header for a nonce, which will enable secure rekeying in the future.
        int rc = sqlite3_key_v2(_sqlDb->getHandle(), nullptr, key.buf, (int)key.size);
        if ( rc != SQLITE_OK ) {
            error::_throw(error::UnsupportedEncryption, "Unable to set encryption key (SQLite error %d)", rc);
        }

        // Since sqlite3_key_v2() does NOT attempt to read the database, we must do our own
        // verification that the encryption key is correct (or db is unencrypted, if no key given):
        rc = sqlite3_exec(_sqlDb->getHandle(), "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr);
        switch ( rc ) {
            case SQLITE_OK:
                return true;
            case SQLITE_NOTADB:
                logError("Could not decrypt database with %s", kAlgorithmName[alg]);
                return false;
            default:
                logError("Could not read database (err %d) using %s", rc, kAlgorithmName[alg]);
                error::_throw(error::SQLite, rc);
        }
    }
#endif


    void SQLiteDataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
#ifdef COUCHBASE_ENTERPRISE
        if ( !factory().encryptionEnabled(alg) ) error::_throw(error::UnsupportedEncryption);

        bool currentlyEncrypted = (options().encryptionAlgorithm != kNoEncryption);
        if ( alg == kNoEncryption ) {
            if ( !currentlyEncrypted ) return;
            logInfo("Decrypting DataFile");
        } else {
            if ( currentlyEncrypted ) {
                logInfo("Changing DataFile encryption key");
            } else {
                logInfo("Encrypting DataFile");
            }
        }

        if ( newKey.size != kEncryptionKeySize[alg] ) error::_throw(error::InvalidParameter);
        int rekeyResult;
        if ( alg == kNoEncryption ) {
            rekeyResult = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, nullptr, 0);
        } else {
            rekeyResult = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, newKey.buf, (int)newKey.size);
        }

        if ( rekeyResult != SQLITE_OK ) { error::_throw(litecore::error::SQLite, rekeyResult); }

        // Update encryption key:
        auto opts                = options();
        opts.encryptionAlgorithm = alg;
        opts.encryptionKey       = newKey;
        setOptions(opts);

        // Finally reopen:
        reopen();
#else
        error::_throw(litecore::error::UnsupportedEncryption);
#endif
    }

    KeyStore* SQLiteDataFile::newKeyStore(const string& name, KeyStore::Capabilities options) {
        Assert(!hasPrefix(name, kDeletedKeyStorePrefix));  // can't access deleted stores directly
        auto keyStore = new SQLiteKeyStore(*this, name, options);
        if ( options.sequences && _schemaVersion >= SchemaVersion::WithDeletedTable
             && keyStoreNameIsCollection(name) ) {
            // Wrap the store in a BothKeyStore that manages it and the deleted store:
            auto deletedStore = new SQLiteKeyStore(*this, kDeletedKeyStorePrefix + name, options);

            // Create a SQLite view of a union of both stores, for use in queries:
#define COLUMNS "key,sequence,flags,version,body,extra,expiration"
            // Invarient: keyStore->tablaName()     == kv_<tableName>
            //            deletedStore->tableName() == kv_del_<tableName>
            //            all_<cname>               == all_<tableName>
            string      tableName = keyStore->tableName().substr(3);  // remove prefix "kv_"
            const char* cname     = tableName.c_str();
            _exec(stringprintf("CREATE TEMP VIEW IF NOT EXISTS \"all_%s\" (" COLUMNS ") AS "
                               "SELECT " COLUMNS " from \"kv_%s\" UNION ALL "
                               "SELECT " COLUMNS " from \"kv_del_%s\"",
                               cname, cname, cname));
#undef COLUMNS

            return new BothKeyStore(keyStore, deletedStore);

        } else {
            return keyStore;
        }
    }

    SQLiteKeyStore* SQLiteDataFile::asSQLiteKeyStore(KeyStore* ks) {
        if ( auto both = dynamic_cast<BothKeyStore*>(ks) ) ks = both->liveStore();
        auto sqlks = dynamic_cast<SQLiteKeyStore*>(ks);
        Assert(sqlks, "Unexpected type of KeyStore");
        return sqlks;
    }

    void SQLiteDataFile::_beginTransaction(ExclusiveTransaction*) {
        checkOpen();
        _exec("BEGIN");
    }

    void SQLiteDataFile::_endTransaction(ExclusiveTransaction* t, bool commit) {
        // Notify key-stores so they can save state:
        forOpenKeyStores([commit](KeyStore& ks) { ks.transactionWillEnd(commit); });

        exec(commit ? "COMMIT" : "ROLLBACK");
    }

    void SQLiteDataFile::beginReadOnlyTransaction() {
        checkOpen();
        _exec("SAVEPOINT roTransaction");
    }

    void SQLiteDataFile::endReadOnlyTransaction() { _exec("RELEASE SAVEPOINT roTransaction"); }

    int SQLiteDataFile::_exec(const string& sql) {
        LogTo(SQL, "%s", sql.c_str());
        try {
            return _sqlDb->exec(sql);
        } catch ( const SQLite::Exception& x ) {
            if ( x.getErrorCode() == SQLITE_ERROR ) {
                // Should we also require that the message contains "syntax error"?
                throw SQLite::Exception(string(x.what()) + " -- " + sql, x.getErrorCode(), x.getExtendedErrorCode());
            } else {
                throw;
            }
        }
    }

    int SQLiteDataFile::exec(const string& sql) {
        if ( !inTransaction() ) error::_throw(error::NotInTransaction);
        return _exec(sql);
    }

    int SQLiteDataFile::execWithLock(const string& sql) {
        checkOpen();
        int result;
        withFileLock([&] { result = _exec(sql); });
        return result;
    }

    int64_t SQLiteDataFile::intQuery(const char* query) {
        SQLite::Statement st(*_sqlDb, query);
        LogStatement(st);
        return st.executeStep() ? st.getColumn(0) : 0;
    }

    unique_ptr<SQLite::Statement> SQLiteDataFile::compile(const char* sql) const {
        checkOpen();
        try {
            LogTo(SQL, "Compiling SQL \"%s\"", sql);
            return make_unique<SQLite::Statement>(*_sqlDb, sql, true);
        } catch ( const SQLite::Exception& x ) {
            warn("SQLite error compiling statement \"%s\": %s", sql, x.what());
            throw;
        }
    }

    void SQLiteDataFile::compileCached(unique_ptr<SQLite::Statement>& ref, const char* sql) const {
        if ( ref == nullptr ) ref = compile(sql);
        else
            checkOpen();
    }

    bool SQLiteDataFile::getSchema(const string& name, const string& type, const string& tableName,
                                   string& outSQL) const {
        SQLite::Statement check(*_sqlDb, "SELECT sql FROM sqlite_master "
                                         "WHERE name = ? AND type = ? AND tbl_name = ?");
        check.bind(1, name);
        check.bind(2, type);
        check.bind(3, tableName);
        LogStatement(check);
        if ( !check.executeStep() ) return false;
        outSQL = check.getColumn(0).getString();
        return true;
    }

    bool SQLiteDataFile::tableExists(const string& name) const {
        const string* checkName = &name;
        string        deletedTableName;
        if ( ((string_view)name).substr(0, 4) == "all_" ) {
            // "all_xxx" is a TEMP VIEW that is not visible to getSchema. Let's
            // check the deleted-table name.
            deletedTableName = "kv_del_";
            deletedTableName += name.substr(4);
            checkName = &deletedTableName;
        }
        string finalName = SQLiteKeyStore::transformCollectionName(*checkName, true);
        string sql;
        return getSchema(finalName, "table", finalName, sql);
    }

    // Returns true if an index/table exists in the database with the given type and SQL schema OR
    // Returns true if the given sql is empty and the schema doesn't exist.
    bool SQLiteDataFile::schemaExistsWithSQL(const string& name, const string& type, const string& tableName,
                                             const string& sql) const {
        string existingSQL;
        bool   existed = getSchema(name, type, tableName, existingSQL);
        if ( !sql.empty() ) return existed && existingSQL == sql;
        else
            return !existed;
    }

    sequence_t SQLiteDataFile::lastSequence(const string& keyStoreName) const {
        sequence_t seq = 0_seq;
        compileCached(_getLastSeqStmt, "SELECT lastSeq FROM kvmeta WHERE name=?");
        UsingStatement u(_getLastSeqStmt);
        _getLastSeqStmt->bindNoCopy(1, keyStoreName);
        if ( _getLastSeqStmt->executeStep() ) seq = sequence_t(int64_t(_getLastSeqStmt->getColumn(0)));
        return seq;
    }

    void SQLiteDataFile::setLastSequence(SQLiteKeyStore& store, sequence_t seq) {
        compileCached(_setLastSeqStmt, "INSERT INTO kvmeta (name, lastSeq) VALUES (?, ?) "
                                       "ON CONFLICT (name) "
                                       "DO UPDATE SET lastSeq = excluded.lastSeq");
        UsingStatement u(_setLastSeqStmt);
        _setLastSeqStmt->bindNoCopy(1, store.name());
        _setLastSeqStmt->bind(2, (long long)seq);
        _setLastSeqStmt->exec();
    }

    uint64_t SQLiteDataFile::purgeCount(const std::string& keyStoreName) const {
        uint64_t purgeCnt = 0;
        if ( _schemaVersion >= SchemaVersion::WithPurgeCount ) {
            compileCached(_getPurgeCntStmt, "SELECT purgeCnt FROM kvmeta WHERE name=?");
            UsingStatement u(_getPurgeCntStmt);
            _getPurgeCntStmt->bindNoCopy(1, keyStoreName);
            if ( _getPurgeCntStmt->executeStep() ) { purgeCnt = (int64_t)_getPurgeCntStmt->getColumn(0); }
        }
        return purgeCnt;
    }

    void SQLiteDataFile::setPurgeCount(SQLiteKeyStore& store, uint64_t count) {
        Assert(_schemaVersion >= SchemaVersion::WithPurgeCount);
        compileCached(_setPurgeCntStmt, "INSERT INTO kvmeta (name, purgeCnt) VALUES (?, ?) "
                                        "ON CONFLICT (name) "
                                        "DO UPDATE SET purgeCnt = excluded.purgeCnt");
        UsingStatement u(_setPurgeCntStmt);
        _setPurgeCntStmt->bindNoCopy(1, store.name());
        _setPurgeCntStmt->bind(2, (long long)count);
        _setPurgeCntStmt->exec();
    }

    uint64_t SQLiteDataFile::fileSize() {
        // Move all WAL changes into the main database file, so its size is accurate:
        _exec("PRAGMA wal_checkpoint(FULL)");
        return DataFile::fileSize();
    }

#pragma mark - QUERIES:

    bool SQLiteDataFile::tableNameIsCollection(slice tableName) {
        return tableName.hasPrefix("kv_") && keyStoreNameIsCollection(tableName.from(3));
    }

    bool SQLiteDataFile::keyStoreNameIsCollection(slice ksName) {
        if ( ksName.hasPrefix(kDeletedKeyStorePrefix) ) ksName = ksName.from(kDeletedKeyStorePrefix.size());
        return ksName == kDefaultKeyStoreName || ksName.hasPrefix(KeyStore::kCollectionPrefix);
    }

    // Maps a collection name used in a query (after "FROM..." or "JOIN...") to a table name.
    // (The name might be of the form "scope.collection", which is fine because that's the same
    // encoding as used in table names.)
    // We have two special rules:
    // 1. The name "_" refers to the default collection; this is simpler than "_default" and
    //    means we don't have to imply that CBL 3.0 supports collections.
    // 2. The name of the database also refers to the default collection, because people are
    //    used to using "FROM bucket_name" in Server queries.
    string SQLiteDataFile::collectionTableName(const string& collection, DeletionStatus type) const {
        // This is legal, but in unit tests it indicates I was passed a table name by mistake:
        DebugAssert(!hasPrefix(collection, "kv_"));

        string name;
        if ( type == QueryTranslator::kLiveAndDeletedDocs ) {
            name = "all_";
        } else {
            name = "kv_";
            if ( type == QueryTranslator::kDeletedDocs ) name += kDeletedKeyStorePrefix;
        }

        auto [scope, coll] = splitCollectionPath(collection);

        if ( collection == "_" || (isDefaultScope(scope) && isDefaultCollection(coll)) ) {
            name += kDefaultKeyStoreName;
        } else if ( !scope && coll == delegate()->databaseName()
                    && !tableExists(name + string(KeyStore::kCollectionPrefix) + coll.asString()) ) {
            // The name of this database represents the default collection,
            // _unless_ there is a collection with that name.
            name += kDefaultKeyStoreName;
        } else {
            string candidate = name + string(KeyStore::kCollectionPrefix);
            bool   isValid   = true;
            if ( !isDefaultScope(scope) ) {
                if ( !KeyStore::isValidCollectionName(scope) ) {
                    isValid = false;
                } else {
                    candidate += SQLiteKeyStore::transformCollectionName(scope.asString(), true)
                                 + KeyStore::kScopeCollectionSeparator;
                }
            }
            if ( isValid && KeyStore::isValidCollectionName(coll) ) {
                candidate += SQLiteKeyStore::transformCollectionName(coll.asString(), true);
            } else {
                error::_throw(error::InvalidQuery, "\"%s\" is not a valid collection name", collection.c_str());
            }
            name = candidate;
        }
        return name;
    }

    string SQLiteDataFile::auxiliaryTableName(const string& onTable, slice typeSeparator, const string& property) {
        return onTable + string(typeSeparator) + SQLiteKeyStore::transformCollectionName(property, true);
    }

    string SQLiteDataFile::FTSTableName(const string& onTable, const string& property) const {
        return auxiliaryTableName(onTable, KeyStore::kIndexSeparator, property);
    }

    string SQLiteDataFile::unnestedTableName(const string& onTable, const string& property) const {
        if ( onTable.find(KeyStore::kUnnestSeparator) == string::npos ) {
            return auxiliaryTableName(onTable, KeyStore::kUnnestSeparator, property);
        } else {
            return auxiliaryTableName(onTable, KeyStore::kUnnestLevelSeparator, property);
        }
    }

#ifdef COUCHBASE_ENTERPRISE
    string SQLiteDataFile::predictiveTableName(const string& onTable, const std::string& property) const {
        return auxiliaryTableName(onTable, KeyStore::kPredictSeparator, property);
    }

    static vectorsearch::Metric actual(vectorsearch::Metric m) {
        return (m == vectorsearch::Metric::Default) ? vectorsearch::Metric::Euclidean2 : m;
    }

    string SQLiteDataFile::vectorTableName(const string& onTable, const std::string& expression,
                                           string_view metricName) const {
        if ( auto specp = findIndexOnExpression(expression, IndexSpec::kVector, onTable) ) {
            if ( !metricName.empty() ) {
                // If a metric name is given, verify that it matches the index:
                auto metricp = vectorsearch::MetricNamed(metricName);
                if ( !metricp )
                    error::_throw(error::InvalidQuery,
                                  "in 3rd argument to APPROX_VECTOR_DISTANCE, '%.*s' is not a valid metric name",
                                  int(metricName.size()), metricName.data());
                auto realMetric = actual(specp->vectorOptions()->metric);
                if ( actual(*metricp) != realMetric ) {
                    string_view realName = vectorsearch::NameOfMetric(realMetric);
                    error::_throw(
                            error::InvalidQuery,
                            "in 3rd argument to APPROX_VECTOR_DISTANCE, %.*s does not match the index's metric, %.*s",
                            int(metricName.size()), metricName.data(), int(realName.size()), realName.data());
                }
            }
            return specp->indexTableName;
        } else {
            error::_throw(error::NoSuchIndex, "vector search with APPROX_VECTOR_DISTANCE requires a vector index on %s",
                          expression.c_str());
        }
    }
#endif


#pragma mark - MAINTENANCE:

    void SQLiteDataFile::_optimize() {
        /* "The optimize pragma is usually a no-op but it will occasionally run ANALYZE if it
            seems like doing so will be useful to the query planner. The analysis_limit pragma
            limits the scope of any ANALYZE command that the optimize pragma runs so that it does
            not consume too many CPU cycles. The constant "400" can be adjusted as needed. Values
            between 100 and 1000 work well for most applications."
            -- <https://sqlite.org/lang_analyze.html> */
        bool logged = false;
        if ( SQL.willLog(LogLevel::Verbose) ) {
            // Log the details of what the optimize will do, before actually doing it:
            SQLite::Statement stmt(*_sqlDb, "PRAGMA analysis_limit=400; PRAGMA optimize(-1)");
            while ( stmt.executeStep() ) {
                LogVerbose(SQL, "PRAGMA optimize ... %s", stmt.getColumn(0).getString().c_str());
                logged = true;
            }
        }
        if ( !logged ) LogVerbose(SQL, "PRAGMA analysis_limit=400; PRAGMA optimize");
        _sqlDb->exec("PRAGMA analysis_limit=400; PRAGMA optimize");
    }

    void SQLiteDataFile::optimize() noexcept {
        try {
            _optimize();
        } catch ( const SQLite::Exception& x ) { warn("Caught SQLite exception while optimizing: %s", x.what()); }
    }

    void SQLiteDataFile::_vacuum(bool always) {
        // <https://blogs.gnome.org/jnelson/2015/01/06/sqlite-vacuum-and-auto_vacuum/>
        int64_t pageCount = intQuery("PRAGMA page_count");
        int64_t freePages = intQuery("PRAGMA freelist_count");
        logVerbose("Housekeeping: %lld of %lld pages free (%.0f%%)", (long long)freePages, (long long)pageCount,
                   100.0 * static_cast<double>(freePages) / static_cast<double>(pageCount));

        if ( !always && (pageCount == 0 || (float)freePages / static_cast<float>(pageCount) < kVacuumFractionThreshold)
             && (freePages * kPageSize < kVacuumSizeThreshold) )
            return;

        string sql;
        bool   fixAutoVacuum = (always || (pageCount * kPageSize) < 10 * MB) && (intQuery("PRAGMA auto_vacuum") == 0);
        if ( fixAutoVacuum ) {
            // Due to issue CBL-707, auto-vacuum did not take effect when creating databases.
            // To enable auto-vacuum on an already-created db, you have to first invoke the
            // pragma and then run a full VACUUM.
            logInfo("Running one-time full VACUUM ... this may take a while [CBL-707]");
            sql = "PRAGMA auto_vacuum=incremental; VACUUM";
        } else {
            logInfo("Incremental-vacuuming database...");
            sql = "PRAGMA incremental_vacuum";
        }

        // On explicit compact, truncate the WAL file to save disk space:
        if ( always ) sql += "; PRAGMA wal_checkpoint(TRUNCATE)";

        fleece::Stopwatch st;
        _exec(sql);
        auto elapsed = st.elapsed();

        int64_t shrunk = pageCount - intQuery("PRAGMA page_count");
        logInfo("    ...removed %" PRIi64 " pages (%" PRIi64 "KB) in %.3f sec", shrunk, shrunk * kPageSize / 1024,
                elapsed);

        if ( fixAutoVacuum && intQuery("PRAGMA auto_vacuum") == 0 )
            warn("auto_vacuum mode did not take effect after running full VACUUM!");
    }

    void SQLiteDataFile::vacuum(bool always) noexcept {
        try {
            _vacuum(always);
        } catch ( const SQLite::Exception& x ) { warn("Caught SQLite exception while vacuuming: %s", x.what()); }
    }

    void SQLiteDataFile::integrityCheck() {
        fleece::Stopwatch st;
        _exec("PRAGMA integrity_check");
        SQLite::Statement stmt(*_sqlDb, "PRAGMA integrity_check");
        stringstream      errors;
        while ( stmt.executeStep() ) {
            if ( string row = stmt.getColumn(0).getString(); row != "ok" ) {
                errors << "\n" << row;
                warn("Integrity check: %s", row.c_str());
            }
        }
        auto elapsed = st.elapsed();
        logInfo("Integrity check took %.3f sec", elapsed);

        if ( string error = errors.str(); !error.empty() )
            error::_throw(error::CorruptData, "Database integrity check failed (details below)%s", error.c_str());
    }

    void SQLiteDataFile::maintenance(MaintenanceType what) {
        checkOpen();
        switch ( what ) {
            case kCompact:
                withFileLock([this]() {
                    _optimize();
                    _vacuum(true);
                });
                break;
            case kReindex:
                execWithLock("REINDEX");
                break;
            case kIntegrityCheck:
                integrityCheck();
                break;
            case kQuickOptimize:
                /* "The analysis_limit pragma limits the scope of any ANALYZE command that the
                    optimize pragma runs so that it does not consume too many CPU cycles.
                    The constant "400" can be adjusted as needed. Values between 100 and 1000 work
                    well for most applications." */
                execWithLock("PRAGMA analysis_limit=400; ANALYZE");
                break;
            case kFullOptimize:
                /* "...to disable the analysis limit, causing ANALYZE to do a complete scan of each
                    index, set the analysis limit to 0." */
                execWithLock("PRAGMA analysis_limit=0; ANALYZE");
                break;
            default:
                error::_throw(error::UnsupportedOperation);
        }
    }

    alloc_slice SQLiteDataFile::rawQuery(const string& query) {
        SQLite::Statement stmt(*_sqlDb, query);
        int               nCols = stmt.getColumnCount();
        fleece::Encoder   enc;
        enc.beginArray();
        while ( stmt.executeStep() ) {
            enc.beginArray();
            for ( int i = 0; i < nCols; ++i ) {
                SQLite::Column col = stmt.getColumn(i);
                switch ( col.getType() ) {
                    case SQLITE_NULL:
                        enc.writeNull();
                        break;
                    case SQLITE_INTEGER:
                        enc.writeInt(col.getInt64());
                        break;
                    case SQLITE_FLOAT:
                        enc.writeDouble(col.getDouble());
                        break;
                    case SQLITE_TEXT:
                        enc.writeString(col.getString());
                        break;
                    case SQLITE_BLOB:
                        enc.writeData(slice(col.getBlob(), col.getBytes()));
                        break;
                }
            }
            enc.endArray();
        }
        enc.endArray();
        return enc.finish();
    }

    alloc_slice SQLiteDataFile::rawScalarQuery(const string& query) {
        SQLite::Statement stmt(*_sqlDb, query);
        std::stringstream ss;

        if ( !stmt.executeStep() ) return nullslice;

        switch ( SQLite::Column col = stmt.getColumn(0); col.getType() ) {
            case SQLITE_NULL:
                return nullslice;
            case SQLITE_INTEGER:
                ss << col.getInt64();
                break;
            case SQLITE_FLOAT:
                ss << col.getDouble();
                break;
            case SQLITE_TEXT:
                return alloc_slice(col.getString());
            case SQLITE_BLOB:
                return {col.getBlob(), static_cast<size_t>(col.getBytes())};
            default:
                break;
        }

        std::string res = ss.str();
        return alloc_slice(res);
    }

    int SQLiteDataFile::defaultMmapSize() { return kMMapSize; }

}  // namespace litecore
