//
// SQLiteKeyStore.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Record.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "sqlite3.h"
#include <sstream>

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {


    vector<string> SQLiteDataFile::allKeyStoreNames() const {
        checkOpen();
        vector<string>    names;
        SQLite::Statement allStores(*_sqlDb, string("SELECT substr(name,4) FROM sqlite_master"
                                                    " WHERE type='table' AND name GLOB 'kv_*'"
                                                    " AND NOT name GLOB 'kv_del_*'"));
        LogStatement(allStores);
        while ( allStores.executeStep() ) {
            string storeName = allStores.getColumn(0).getString();
            names.push_back(SQLiteKeyStore::transformCollectionName(storeName, false));
        }

        return names;
    }

    void SQLiteDataFile::deleteKeyStore(const std::string& name) {
        exec("DROP TABLE IF EXISTS \"kv_" + SQLiteKeyStore::transformCollectionName(name, true) + "\"");
        exec("DROP TABLE IF EXISTS \"kv_del_" + SQLiteKeyStore::transformCollectionName(name, true) + "\"");
        // TODO: Do I need to drop indexes, triggers?
    }

    KeyStore& SQLiteDataFile::keyStoreFromTable(slice tableName) {
        Assert(tableName == "kv_default" || tableName.hasPrefix("kv_."));
        auto tableName_ = string(tableName.from(3));
        return getKeyStore(SQLiteKeyStore::transformCollectionName(tableName_, false));
    }

    bool SQLiteDataFile::keyStoreExists(const string& name) const { return tableExists(string("kv_") + name); }

    SQLiteKeyStore::SQLiteKeyStore(SQLiteDataFile& db, const string& name, KeyStore::Capabilities capabilities)
        : KeyStore(db, name, capabilities)
        , _tableName("kv_" + transformCollectionName(name, true))
        , _quotedTableName("\"" + _tableName + "\"") {
        reopen();
    }

    void SQLiteKeyStore::createTable() {
        // Here's the table schema. The body comes last because it may be very large, and it's
        // more efficient in SQLite to keep large columns at the end of a row.
        // Create the sequence and flags columns regardless of options, otherwise it's too
        // complicated to customize all the SQL queries to conditionally use them...
        db().execWithLock(subst("CREATE TABLE IF NOT EXISTS kv_@ ("
                                "  key TEXT PRIMARY KEY,"
                                "  sequence INTEGER,"
                                "  flags INTEGER DEFAULT 0,"
                                "  version BLOB,"
                                "  body BLOB,"
                                "  extra BLOB)"));
        _uncommitedTable = db().inTransaction();
    }

    void SQLiteKeyStore::close() {
        // If statements are left open, closing the database will fail with a "db busy" error...
        _stmtCache.clear();
        KeyStore::close();
    }

    void SQLiteKeyStore::reopen() {
        if ( !db().keyStoreExists(_name) ) createTable();
    }

    // Replaces `kv_@`, *with a preceding `"`*, with the unquoted table name.
    // Replaces other `kv_@` with the quoted table name.
    string SQLiteKeyStore::subst(const char* sqlTemplate) const {
        string sql(sqlTemplate);
        replace(sql, "\"kv_@", "\"" + tableName());
        replace(sql, "kv_@", quotedTableName());
        return sql;
    }

    std::unique_ptr<SQLite::Statement> SQLiteKeyStore::compile(const char* sql) const { return db().compile(sql); }

    SQLite::Statement& SQLiteKeyStore::compileCached(const string& sqlTemplate) const {
        auto i = _stmtCache.find(sqlTemplate);
        if ( i == _stmtCache.end() ) {
            // Note: Substituting the store name for "@" in the SQL
            auto stmt = db().compile(subst(sqlTemplate.c_str()).c_str());
            i         = _stmtCache.insert({sqlTemplate, std::move(stmt)}).first;
        } else {
            db().checkOpen();
        }
        return *i->second;
    }

    uint64_t SQLiteKeyStore::recordCount(bool includeDeleted) const {
        auto&          stmt = compileCached(includeDeleted ? "SELECT count(*) FROM kv_@"
                                                           : "SELECT count(*) FROM kv_@ WHERE (flags & 1) != 1");
        UsingStatement u(stmt);
        if ( stmt.executeStep() ) return (int64_t)stmt.getColumn(0);
        return 0;
    }

    void SQLiteKeyStore::shareSequencesWith(KeyStore& source) {
        _sequencesOwner = dynamic_cast<SQLiteKeyStore*>(&source);
    }

    sequence_t SQLiteKeyStore::lastSequence() const {
        if ( _sequencesOwner ) {
            return _sequencesOwner->lastSequence();
        } else {
            if ( _lastSequence ) return *_lastSequence;
            sequence_t seq = db().lastSequence(_name);
            if ( db().inTransaction() ) _lastSequence = seq;
            return seq;
        }
    }

    void SQLiteKeyStore::setLastSequence(sequence_t seq) {
        if ( _sequencesOwner ) {
            _sequencesOwner->setLastSequence(seq);
        } else {
            if ( _capabilities.sequences ) {
                _lastSequence        = seq;
                _lastSequenceChanged = true;
            }
        }
    }

    uint64_t SQLiteKeyStore::purgeCount() const {
        if ( _purgeCountValid ) return _purgeCount;
        uint64_t cnt = db().purgeCount(_name);
        if ( db().inTransaction() ) {
            _purgeCount      = cnt;
            _purgeCountValid = true;
        }

        return cnt;
    }

    void SQLiteKeyStore::incrementPurgeCount() {
        ++_purgeCount;
        _purgeCountChanged = true;
    }

    void SQLiteKeyStore::transactionWillEnd(bool commit) {
        if ( _lastSequenceChanged ) {
            Assert(!_sequencesOwner);
            if ( commit ) db().setLastSequence(*this, *_lastSequence);
            _lastSequenceChanged = false;
        }

        if ( _purgeCountChanged ) {
            if ( commit ) db().setPurgeCount(*this, _purgeCount);
            _purgeCountChanged = false;
        }

        _lastSequence    = nullopt;
        _purgeCountValid = false;

        if ( !commit ) {
            if ( _uncommittedExpirationColumn ) _hasExpirationColumn = false;
            if ( _uncommitedTable ) { close(); }
        }

        _uncommittedExpirationColumn = false;
        _uncommitedTable             = false;
    }

    /*static*/ slice SQLiteKeyStore::columnAsSlice(const SQLite::Column& col) {
        return {col.getBlob(), static_cast<size_t>(col.getBytes())};
    }

    // The columns in `stmt` must match RecordColumn.
    /*static*/ void SQLiteKeyStore::setRecordMetaAndBody(Record& rec, SQLite::Statement& stmt, ContentOption content,
                                                         bool setKey, bool setSequence) {
        rec.setExists();
        rec.setContentLoaded(content);
        if ( setKey ) rec.setKey(getColumnAsSlice(stmt, RecordColumn::Key));
        if ( setSequence ) rec.updateSequence(sequence_t(int64_t(stmt.getColumn(RecordColumn::Sequence))));

        // The subsequence is in the `flags` column, left-shifted so it doesn't interfere with the
        // defined flag bits.
        int64_t rawFlags = stmt.getColumn(RecordColumn::RawFlags);
        rec.setFlags(DocumentFlags(rawFlags & 0xFFFF));
        rec.updateSubsequence(rawFlags >> 16);

        rec.setVersion(getColumnAsSlice(stmt, RecordColumn::Version));

        if ( content == kMetaOnly ) rec.setUnloadedBodySize((ssize_t)stmt.getColumn(RecordColumn::BodyOrSize));
        else
            rec.setBody(getColumnAsSlice(stmt, RecordColumn::BodyOrSize));

        if ( content >= kEntireBody ) rec.setExtra(getColumnAsSlice(stmt, RecordColumn::ExtraOrSize));
        else
            rec.setUnloadedExtraSize((ssize_t)stmt.getColumn(RecordColumn::ExtraOrSize));
    }

    string SQLiteKeyStore::transformCollectionName(const string& name, bool mangle) {
        ostringstream ss;
        const char*   name_cstr = name.c_str();
        while ( char c = *name_cstr++ ) {
            if ( c == '\\' ) { continue; }

            if ( mangle ) {
                if ( c >= 'A' && c <= 'Z' ) { ss << '\\'; }
            }

            ss << c;
        }

        return ss.str();
    }

    bool SQLiteKeyStore::read(Record& rec, ReadBy by, ContentOption content) const {
        //  This statement does nothing if the sequence index has already been created.
        if (by == ReadBy::Sequence)
            ((SQLiteKeyStore *)(this))->createSequenceIndex();

        // Note: In this SELECT statement the result column order must match RecordColumn.
        string sql;
        sql.reserve(100);
        sql = (by == ReadBy::Key) ? "SELECT sequence, flags, null, version" : "SELECT null, flags, key, version";
        sql += (content >= kCurrentRevOnly) ? ", body" : ", length(body)";
        sql += (content >= kEntireBody) ? ", extra" : ", length(extra)";
        sql += " FROM kv_@ WHERE ";
        sql += (by == ReadBy::Key) ? "key=?" : "sequence=?";

        lock_guard<mutex> lock(_stmtMutex);
        auto&             stmt = compileCached(sql);
        if ( by == ReadBy::Key ) {
            DebugAssert(rec.key());
            stmt.bindNoCopy(1, (const char*)rec.key().buf, (int)rec.key().size);
        } else {
            DebugAssert(rec.sequence() != 0_seq);
            stmt.bind(1, (long long)rec.sequence());
        }

        UsingStatement u(stmt);
        if ( !stmt.executeStep() ) return false;
        setRecordMetaAndBody(rec, stmt, content, (by != ReadBy::Key), (by != ReadBy::Sequence));
        return true;
    }

    void SQLiteKeyStore::setKV(slice key, slice version, slice value, ExclusiveTransaction& t) {
        DebugAssert(key.size > 0);
        DebugAssert(!_capabilities.sequences);
        if ( db().willLog(LogLevel::Verbose) && name() != "default" )
            db()._logVerbose("KeyStore(%-s) set '%.*s'", name().c_str(), SPLAT(key));

        enum { KeyParam = 1, VersionParam, BodyParam };

        auto&          stmt = compileCached("INSERT OR REPLACE INTO kv_@ (key, version, body) VALUES (?, ?, ?)");
        UsingStatement u(stmt);
        stmt.bindNoCopy(KeyParam, (const char*)key.buf, (int)key.size);
        stmt.bindNoCopy(VersionParam, version.buf, (int)version.size);
        stmt.bindNoCopy(BodyParam, value.buf, (int)value.size);
        stmt.exec();
    }

    sequence_t SQLiteKeyStore::set(const RecordUpdate& rec, SetOptions flags, ExclusiveTransaction&) {
        DebugAssert(rec.key.size > 0);
        DebugAssert(_capabilities.sequences);

        // About subsequences: Rather than adding another column, we store the subsequence in the
        // `flags` column, left-shifted so it doesn't interfere with the defined flag bits.

        enum {
            VersionParam = 1,
            BodyParam,
            ExtraParam,
            FlagsParam,
            SequenceParam,
            KeyParam,
            OldSequenceParam,
            OldSubsequenceParam
        };

        bool                              tryAgain = false;
        sequence_t                        ret;
        std::tuple<std::string, int, int> lastExcArgs;

        do {
            // This is a band-aid for an undiagnosed bug, that lastSeq stored in the meta
            // table may be short of the largest sequence used in the kv table. c.f. cbl-3612
            if ( tryAgain ) {
                SQLite::Statement& stmt = compileCached("SELECT MAX(sequence) FROM kv_@");
                UsingStatement     u(stmt);
                int64_t            maxSeq = -1;
                if ( stmt.executeStep() ) maxSeq = int64_t(stmt.getColumn(0));

                if ( maxSeq < 0 || (uint64_t)lastSequence() >= maxSeq ) {
                    // rethrow the original exception that gets us here.
                    // The re-try is to fix the situation when lastSequence() lags behind maxSeq
                    throw SQLite::Exception(std::get<0>(lastExcArgs), std::get<1>(lastExcArgs),
                                            std::get<2>(lastExcArgs));
                }

                setLastSequence((sequence_t)maxSeq);
            }

            const char*        opName;
            SQLite::Statement* stmt;
            if ( rec.sequence == 0_seq || flags & SetOptions::kInsert ) {
                // Insert only:
                stmt   = &compileCached("INSERT OR IGNORE INTO kv_@ (version, body, extra, flags, sequence, key)"
                                          " VALUES (?, ?, ?, ?, ?, ?)");
                opName = "insert";
            } else {
                // Replace only:
                stmt = &compileCached("UPDATE kv_@ SET version=?, body=?, extra=?, flags=?, sequence=?"
                                      " WHERE key=? AND sequence=? AND (flags >> 16) = ?");
                stmt->bind(OldSequenceParam, (long long)rec.sequence);
                stmt->bind(OldSubsequenceParam, (long long)rec.subsequence);
                opName = "update";
            }

            sequence_t seq;
            int64_t    rawFlags = int(rec.flags);
            if ( flags & kUpdateSequence ) {
                seq = lastSequence() + 1;
            } else {
                Assert(rec.sequence > 0_seq);
                seq = rec.sequence;
                // If we don't update the sequence, update the subsequence so MVCC can work:
                rawFlags |= static_cast<int64_t>(rec.subsequence + 1) << 16;
            }

            stmt->bindNoCopy(VersionParam, rec.version.buf, (int)rec.version.size);
            stmt->bindNoCopy(BodyParam, rec.body.buf, (int)rec.body.size);
            stmt->bindNoCopy(ExtraParam, rec.extra.buf, (int)rec.extra.size);
            stmt->bind(FlagsParam, (long long)rawFlags);
            stmt->bindNoCopy(KeyParam, (const char*)rec.key.buf, (int)rec.key.size);
            stmt->bind(SequenceParam, (long long)seq);

            if ( db().willLog(LogLevel::Verbose) && name() != "default" )
                db()._logVerbose("KeyStore(%-s) %s %.*s", name().c_str(), opName, SPLAT(rec.key));

            UsingStatement u(*stmt);
            int            status;
            try {
                status = stmt->exec();
                // We are good, don't try again.
                tryAgain = false;
            } catch ( const SQLite::Exception& exc ) {
                // We won't try again twice.
                tryAgain               = !tryAgain;
                int sqliteErrorCode    = exc.getErrorCode();
                int sqliteExtErrorCode = exc.getExtendedErrorCode();
                if ( tryAgain && sqliteErrorCode == SQLITE_CONSTRAINT
                     && sqliteExtErrorCode == SQLITE_CONSTRAINT_UNIQUE ) {
                    // We only handle this error,
                    // Log: Database | UNIQUE constraint failed: kv_default.sequence (19/2067)
                    // Jot down the exception that makes us to try again.
                    lastExcArgs = std::make_tuple(exc.what(), sqliteErrorCode, sqliteExtErrorCode);
                    continue;
                } else {
                    // Otherwise, rethrow.
                    throw;
                }
            }

            // Assertion: tryAgain == false

            if ( status == 0 ) {
                ret = 0_seq;  // condition wasn't met, i.e. conflict
                break;
            }
            if ( flags & kUpdateSequence ) setLastSequence(seq);

            ret = seq;
        } while ( tryAgain );
        return ret;
    }

    bool SQLiteKeyStore::del(slice key, ExclusiveTransaction&, sequence_t seq, std::optional<uint64_t> subseq) {
        Assert(key);
        SQLite::Statement* stmt;
        db()._logVerbose("SQLiteKeyStore(%s) del key '%.*s' seq %" PRIu64, _name.c_str(), SPLAT(key), (uint64_t)seq);
        if ( seq != 0_seq ) {
            if ( subseq ) {
                stmt = &compileCached("DELETE FROM kv_@ WHERE key=? AND sequence=?"
                                      " AND (flags >> 16) = ?");
                stmt->bind(3, (long long)*subseq);
            } else {
                stmt = &compileCached("DELETE FROM kv_@ WHERE key=? AND sequence=?");
            }
            stmt->bind(2, (long long)seq);
        } else {
            stmt = &compileCached("DELETE FROM kv_@ WHERE key=?");
        }
        stmt->bindNoCopy(1, (const char*)key.buf, (int)key.size);
        UsingStatement u(*stmt);
        if ( stmt->exec() == 0 ) return false;

        incrementPurgeCount();
        return true;
    }

    void SQLiteKeyStore::moveTo(slice key, KeyStore& dst, ExclusiveTransaction& t, slice newKey) {
        SQLiteKeyStore* dstStore = SQLiteDataFile::asSQLiteKeyStore(&dst);
        if ( dstStore == this || &dstStore->dataFile() != &dataFile() ) error::_throw(error::InvalidParameter);

        if ( newKey == nullslice ) newKey = key;
        sequence_t seq = dstStore->lastSequence() + 1;

        // ???? Should the version be reset since it's in a new collection?
        auto& stmt = compileCached("INSERT INTO " + dstStore->quotedTableName()
                                   + " (key, version, body, extra, flags, sequence)"
                                     "  SELECT ?, version, body, extra, flags, ? FROM kv_@ WHERE key=?");
        stmt.bindNoCopy(1, (const char*)newKey.buf, (int)newKey.size);
        stmt.bind(2, (long long)seq);
        stmt.bindNoCopy(3, (const char*)key.buf, (int)key.size);
        UsingStatement u(stmt);

        try {
            if ( stmt.exec() == 0 ) error::_throw(error::NotFound);
        } catch ( SQLite::Exception& x ) {
            if ( x.getErrorCode() == SQLITE_CONSTRAINT )  // duplicate key!
                error::_throw(error::Conflict);
            else
                throw;
        }

        dstStore->setLastSequence(seq);

        // Finally delete the old record:
        del(key, t);
    }

    bool SQLiteKeyStore::setDocumentFlag(slice key, sequence_t seq, DocumentFlags flags, ExclusiveTransaction&) {
        // "flags + 0x10000" increments the subsequence stored in the upper bits, for MVCC.
        auto& stmt = compileCached("UPDATE kv_@ SET flags = ((flags + 0x10000) | ?) WHERE key=? AND sequence=?");
        UsingStatement u(stmt);
        stmt.bind(1, (unsigned)flags);
        stmt.bindNoCopy(2, (const char*)key.buf, (int)key.size);
        stmt.bind(3, (long long)seq);
        return stmt.exec() > 0;
    }

    void SQLiteKeyStore::createTrigger(string_view triggerName, string_view triggerSuffix, string_view operation,
                                       string when, string_view statements) {
        if ( hasPrefix(when, "WHERE") ) when.replace(0, 5, "WHEN");
        string sql = CONCAT("CREATE TRIGGER \"" << triggerName << "::" << triggerSuffix << "\" " << operation << " ON "
                                                << quotedTableName() << " " << when << ' ' << " BEGIN " << statements
                                                << "; END");
        LogTo(QueryLog, "    ...for index: %s", sql.c_str());
        db().exec(sql);
    }

    vector<alloc_slice> SQLiteKeyStore::withDocBodies(const vector<slice>& docIDs, WithDocBodyCallback callback) {
        if ( docIDs.empty() ) return {};

        unordered_map<slice, size_t> docIndices;  // maps docID -> index in docIDs[]
        docIndices.reserve(docIDs.size());

        // Construct SQL query with a big "IN (...)" clause for all the docIDs:
        stringstream sql;
        sql << "SELECT key, fl_callback(key, version, body, extra, sequence, flags, ?) FROM " << quotedTableName()
            << " WHERE key IN ('";
        unsigned n = 0;
        for ( slice docID : docIDs ) {
            docIndices.insert({docID, n});
            if ( n++ > 0 ) sql << "','";
            if ( docID.findByte('\'') ) {
                string escaped(docID);
                replace(escaped, "'", "''");
                sql << escaped;
            } else {
                sql << docID;
            }
        }
        sql << "')";

        SQLite::Statement stmt(db(), sql.str());
        LogStatement(stmt);
        stmt.bindPointer(1, &callback, kWithDocBodiesCallbackPointerType);

        // Run the statement and put the results into an array in the same order as docIDs:
        alloc_slice         empty(size_t(0));
        vector<alloc_slice> results(docIDs.size());
        while ( stmt.executeStep() ) {
            slice  docID = getColumnAsSlice(stmt, 0);
            slice  value = getColumnAsSlice(stmt, 1);
            size_t i     = docIndices[docID];
            //Log("    -- %zu: %.*s --> '%.*s'", i, SPLAT(docID), SPLAT(revs));
            if ( value.size == 0 && value.buf != nullptr )
                results[i] = empty;  // reuse one empty slice instead of creating one per row
            else
                results[i] = alloc_slice(value);
        }
        return results;
    }

#pragma mark - EXPIRATION:

    // Returns true if the KeyStore's table has had the 'expiration' column added to it.
    bool SQLiteKeyStore::mayHaveExpiration() {
        if ( !_hasExpirationColumn ) {
            string sql;
            string tableName = this->tableName();
            db().getSchema(tableName, "table", tableName, sql);
            if ( sql.find("expiration") != string::npos ) _hasExpirationColumn = true;
        }
        return _hasExpirationColumn;
    }

    // Adds the 'expiration' column to the table.
    void SQLiteKeyStore::addExpiration() {
        if ( mayHaveExpiration() ) return;
        db()._logVerbose("Adding the `expiration` column & index to kv_%s", name().c_str());
        db().execWithLock(subst("ALTER TABLE kv_@ ADD COLUMN expiration INTEGER; "
                                "CREATE INDEX \"kv_@_expiration\" ON kv_@ (expiration) WHERE expiration not null"));
        _hasExpirationColumn         = true;
        _uncommittedExpirationColumn = true;
    }

    bool SQLiteKeyStore::setExpiration(slice key, expiration_t expTime) {
        Assert(expTime >= expiration_t(0), "Invalid (negative) expiration time");
        addExpiration();
        auto&          stmt = compileCached("UPDATE kv_@ SET expiration=? WHERE key=?");
        UsingStatement u(stmt);
        if ( expTime > expiration_t::None ) stmt.bind(1, (long long)expTime);
        else
            stmt.bind(1);  // null
        stmt.bindNoCopy(2, (const char*)key.buf, (int)key.size);
        bool ok = stmt.exec() > 0;
        if ( ok )
            db()._logVerbose("SQLiteKeyStore(%s) set expiration of '%.*s' to %" PRId64, _name.c_str(), SPLAT(key),
                             (uint64_t)expTime);
        return ok;
    }

    expiration_t SQLiteKeyStore::getExpiration(slice key) {
        if ( !mayHaveExpiration() ) return expiration_t::None;
        auto&          stmt = compileCached("SELECT expiration FROM kv_@ WHERE key=?");
        UsingStatement u(stmt);
        stmt.bindNoCopy(1, (const char*)key.buf, (int)key.size);
        if ( !stmt.executeStep() ) return expiration_t::None;
        return expiration_t(int64_t(stmt.getColumn(0)));
    }

    expiration_t SQLiteKeyStore::nextExpiration() {
        expiration_t next = expiration_t::None;
        if ( mayHaveExpiration() ) {
            auto&          stmt = compileCached("SELECT min(expiration) FROM kv_@ WHERE expiration IS NOT NULL");
            UsingStatement u(stmt);
            if ( !stmt.executeStep() ) return next;
            next = expiration_t(int64_t(stmt.getColumn(0)));
        }
        db()._logVerbose("Next expiration time is %" PRId64, (int64_t)next);
        return next;
    }

    unsigned SQLiteKeyStore::expireRecords(optional<ExpirationCallback> callback) {
        if ( !mayHaveExpiration() ) return 0;
        expiration_t t       = now();
        unsigned     expired = 0;
        bool         none    = false;
        if ( callback ) {
            auto&          stmt = compileCached("SELECT key FROM kv_@ WHERE expiration <= ?");
            UsingStatement u(stmt);
            stmt.bind(1, (long long)t);
            none = true;
            while ( stmt.executeStep() ) {
                none = false;
                (*callback)(getColumnAsSlice(stmt, 0));
            }
        }
        if ( !none ) {
            expired = db().exec(
                    format("DELETE FROM %s WHERE expiration <= %" PRId64, _quotedTableName.c_str(), (int64_t)t));
        }
        db()._logInfo("Purged %u expired documents", expired);
        return expired;
    }

}  // namespace litecore
