//
// SQLiteKeyStore.cc
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

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Record.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "FleeceImpl.hh"
#include <sstream>

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

#if 0 //UNUSED:
    vector<string> SQLiteDataFile::allKeyStoreNames() {
        checkOpen();
        vector<string> names;
        SQLite::Statement allStores(*_sqlDb, string("SELECT substr(name,4) FROM sqlite_master"
                                                    " WHERE type='table' AND name GLOB 'kv_*'"));
        LogStatement(allStores);
        while (allStores.executeStep()) {
            string storeName = allStores.getColumn(0).getString();
            names.push_back(storeName);
        }
        
        return names;
    }
#endif


    bool SQLiteDataFile::keyStoreExists(const string &name) {
        return tableExists(string("kv_") + name);
    }


    SQLiteKeyStore::SQLiteKeyStore(SQLiteDataFile &db, const string &name, KeyStore::Capabilities capabilities)
    :KeyStore(db, name, capabilities)
    {
        if (db.keyStoreExists(name))
            _existence = kCommitted;
        else
            createTable();
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
                                "  body BLOB)"));
        _existence = db().inTransaction() ? kUncommitted : kCommitted;
    }


    void SQLiteKeyStore::close() {
        // If statements are left open, closing the database will fail with a "db busy" error...
        _recCountStmt.reset();
        _getByKeyStmt.reset();
        _getCurByKeyStmt.reset();
        _getMetaByKeyStmt.reset();
        _getBySeqStmt.reset();
        _getCurBySeqStmt.reset();
        _getMetaBySeqStmt.reset();
        _setStmt.reset();
        _insertStmt.reset();
        _replaceStmt.reset();
        _delByKeyStmt.reset();
        _delBySeqStmt.reset();
        _delByBothStmt.reset();
        _setFlagStmt.reset();
        _setExpStmt.reset();
        _getExpStmt.reset();
        _nextExpStmt.reset();
        _findExpStmt.reset();
        _withDocBodiesStmt.reset();
        KeyStore::close();
    }


    void SQLiteKeyStore::reopen() {
        if (_existence == kNonexistent)
            createTable();
    }


    string SQLiteKeyStore::subst(const char *sqlTemplate) const {
        string sql(sqlTemplate);
        size_t pos;
        while(string::npos != (pos = sql.find('@')))
            sql.replace(pos, 1, name());
        return sql;
    }


    SQLite::Statement* SQLiteKeyStore::compile(const string &sql) const {
        try {
            return new SQLite::Statement(db(), sql, true);
        } catch (const SQLite::Exception &x) {
            db().warn("SQLite error compiling statement \"%s\": %s", sql.c_str(), x.what());
            throw;
        }
    }


    SQLite::Statement& SQLiteKeyStore::compile(const unique_ptr<SQLite::Statement>& ref,
                                               const char *sqlTemplate) const
    {
        if (ref != nullptr) {
            db().checkOpen();
            return *ref.get();
        } else {
            return db().compile(ref, subst(sqlTemplate).c_str());
        }
    }


    uint64_t SQLiteKeyStore::recordCount() const {
        if (!_recCountStmt) {
            stringstream sql;
            sql << "SELECT count(*) FROM kv_" << _name << " WHERE (flags & 1) != 1";
            compile(_recCountStmt, sql.str().c_str());
        }
        UsingStatement u(_recCountStmt);
        if (_recCountStmt->executeStep()) {
            auto count = (int64_t)_recCountStmt->getColumn(0);
            return count;
        }
        return 0;
    }


    sequence_t SQLiteKeyStore::lastSequence() const {
        if (_lastSequence >= 0)
            return _lastSequence;
        sequence_t seq = db().lastSequence(_name);
        if (db().inTransaction())
            _lastSequence = seq;
        return seq;
    }

    
    void SQLiteKeyStore::setLastSequence(sequence_t seq) {
        if (_capabilities.sequences) {
            _lastSequence = seq;
            _lastSequenceChanged = true;
        }
    }


    uint64_t SQLiteKeyStore::purgeCount() const {
        if(_purgeCountValid)
            return _purgeCount;
        uint64_t cnt = db().purgeCount(_name);
        if(db().inTransaction()) {
            _purgeCount = cnt;
            _purgeCountValid = true;
        }

        return cnt;
    }

    void SQLiteKeyStore::incrementPurgeCount() {
        ++_purgeCount;
        _purgeCountChanged = true;
    }


    void SQLiteKeyStore::transactionWillEnd(bool commit) {
        if (_lastSequenceChanged) {
            if (commit)
                db().setLastSequence(*this, _lastSequence);
            _lastSequenceChanged = false;
        }

        if(_purgeCountChanged) {
            if(commit)
                db().setPurgeCount(*this, _purgeCount);
            _purgeCountChanged = false;
        }

        _lastSequence = -1;
        _purgeCountValid = false;

        if (!commit && _uncommittedExpirationColumn)
            _hasExpirationColumn = false;
        _uncommittedExpirationColumn = false;

        if (_existence == kUncommitted) {
            if (commit) {
                _existence = kCommitted;
            } else {
                _existence = kNonexistent;
                close();
                return;
            }
        }
    }


    /*static*/ slice SQLiteKeyStore::columnAsSlice(const SQLite::Column &col) {
        return slice(col.getBlob(), col.getBytes());
    }

    static slice textColumnAsSlice(const SQLite::Column &col) {
        return slice(col.getText(nullptr), col.getBytes());
    }


    // OPT: Would be nice to avoid copying key/vers/body here; this would require Record to
    // know that the pointers are ephemeral, and create copies if they're accessed as
    // alloc_slice (not just slice).


    // Gets flags from col 1, version from col 3, and body (or its length) from col 4
    /*static*/ void SQLiteKeyStore::setRecordMetaAndBody(Record &rec,
                                                         SQLite::Statement &stmt,
                                                         ContentOption content)
    {
        rec.setExists();
        rec.setFlags((DocumentFlags)(int)stmt.getColumn(1));
        rec.setVersion(columnAsSlice(stmt.getColumn(3)));
        if (content == kMetaOnly)
            rec.setUnloadedBodySize((ssize_t)stmt.getColumn(4));
        else
            rec.setBody(columnAsSlice(stmt.getColumn(4)));
    }
    

    bool SQLiteKeyStore::read(Record &rec, ContentOption content) const {
        SQLite::Statement *stmt;
        switch (content) {
            case kMetaOnly:
                stmt = &compile(_getMetaByKeyStmt,
                        "SELECT sequence, flags, 0, version, length(body) FROM kv_@ WHERE key=?");
                break;
            case kCurrentRevOnly:
                stmt = &compile(_getCurByKeyStmt,
                        "SELECT sequence, flags, 0, version, fl_root(body) FROM kv_@ WHERE key=?");
                break;
            case kEntireBody:
                stmt = &compile(_getByKeyStmt,
                        "SELECT sequence, flags, 0, version, body FROM kv_@ WHERE key=?");
                break;
            default:
                return false;
        }

        {
            lock_guard<mutex> lock(_stmtMutex);
            stmt->bindNoCopy(1, (const char*)rec.key().buf, (int)rec.key().size);
            UsingStatement u(*stmt);
            if (!stmt->executeStep())
                return false;

            sequence_t seq = (int64_t)stmt->getColumn(0);
            rec.updateSequence(seq);
            setRecordMetaAndBody(rec, *stmt, content);
        }
        return true;
    }


    Record SQLiteKeyStore::get(sequence_t seq /*, ContentOptions content*/) const {
        constexpr ContentOption content = kEntireBody;  // this used to be a param but not used
        Assert(_capabilities.sequences);
        Record rec;
        SQLite::Statement *stmt;
        switch (content) {
            case kMetaOnly:
                stmt = &compile(_getMetaBySeqStmt,
                        "SELECT 0, flags, key, version, length(body) FROM kv_@ WHERE sequence=?");
                break;
            case kCurrentRevOnly:
                stmt = &compile(_getCurBySeqStmt,
                        "SELECT 0, flags, key, version, fl_root(body) FROM kv_@ WHERE sequence=?");
                break;
            case kEntireBody:
                stmt = &compile(_getBySeqStmt,
                        "SELECT 0, flags, key, version, body FROM kv_@ WHERE sequence=?");
                break;
            default:
                error::_throw(error::UnexpectedError);
        }

        UsingStatement u(*stmt);
        stmt->bind(1, (long long)seq);
        if (stmt->executeStep()) {
            rec.setKey(columnAsSlice(stmt->getColumn(2)));
            rec.updateSequence(seq);
            setRecordMetaAndBody(rec, *stmt, content);
        }
        return rec;
    }


    sequence_t SQLiteKeyStore::set(slice key, slice vers, slice body, DocumentFlags flags,
                                   Transaction&,
                                   const sequence_t *replacingSequence,
                                   bool newSequence)
    {
        const char *opName;
        SQLite::Statement *stmt;
        if (replacingSequence == nullptr) {
            // Default:
            compile(_setStmt,
                    "INSERT OR REPLACE INTO kv_@ (version, body, flags, sequence, key)"
                    " VALUES (?, ?, ?, ?, ?)");
            stmt = _setStmt.get();
            opName = "set";
        } else if (*replacingSequence == 0) {
            // Insert only:
            compile(_insertStmt,
                    "INSERT OR IGNORE INTO kv_@ (version, body, flags, sequence, key)"
                    " VALUES (?, ?, ?, ?, ?)");
            stmt = _insertStmt.get();
            opName = "insert";
        } else {
            // Replace only:
            Assert(_capabilities.sequences);
            compile(_replaceStmt,
                    "UPDATE kv_@ SET version=?, body=?, flags=?, sequence=?"
                    " WHERE key=? AND sequence=?");
            stmt = _replaceStmt.get();
            stmt->bind(6, (long long)*replacingSequence);
            opName = "update";
        }
        stmt->bindNoCopy(1, vers.buf, (int)vers.size);
        stmt->bindNoCopy(2, body.buf, (int)body.size);
        stmt->bind(3, (int)flags);
        stmt->bindNoCopy(5, (const char*)key.buf, (int)key.size);

        sequence_t seq = 0;
        if (_capabilities.sequences) {
            if (newSequence) {
                seq = lastSequence() + 1;
            } else {
                Assert(replacingSequence && *replacingSequence > 0);
                seq = *replacingSequence;
            }
            stmt->bind(4, (long long)seq);
        } else {
            stmt->bind(4); // null
            seq = 1;
        }

        if (db().willLog(LogLevel::Verbose) && name() != "default")
            db()._logVerbose("KeyStore(%-s) %s %.*s", name().c_str(), opName, SPLAT(key));

        UsingStatement u(*stmt);
        if (stmt->exec() == 0)
            return 0;               // condition wasn't met

        if (_capabilities.sequences && newSequence)
            setLastSequence(seq);
        return seq;
    }


    bool SQLiteKeyStore::del(slice key, Transaction&, sequence_t seq) {
        Assert(key);
        SQLite::Statement *stmt;
        db()._logVerbose("SQLiteKeyStore(%s) del key '%.*s' seq %" PRIu64,
                        _name.c_str(), SPLAT(key), seq);
        if (seq) {
            stmt = &compile(_delByBothStmt, "DELETE FROM kv_@ WHERE key=? AND sequence=?");
            stmt->bind(2, (long long)seq);
        } else {
            stmt = &compile(_delByKeyStmt, "DELETE FROM kv_@ WHERE key=?");
        }
        stmt->bindNoCopy(1, (const char*)key.buf, (int)key.size);
        UsingStatement u(*stmt);
        if(stmt->exec() == 0)
            return false;

        incrementPurgeCount();
        return true;
    }


    bool SQLiteKeyStore::setDocumentFlag(slice key, sequence_t seq, DocumentFlags flags,
                                         Transaction&)
    {
        compile(_setFlagStmt, "UPDATE kv_@ SET flags=(flags | ?) WHERE key=? AND sequence=?");
        UsingStatement u(*_setFlagStmt);
        _setFlagStmt->bind      (1, (unsigned)flags);
        _setFlagStmt->bindNoCopy(2, (const char*)key.buf, (int)key.size);
        _setFlagStmt->bind      (3, (long long)seq);
        return _setFlagStmt->exec() > 0;
    }


    void SQLiteKeyStore::erase() {
        Transaction t(db());
        db().exec(string("DELETE FROM kv_"+name()));
        setLastSequence(0);
        t.commit();
    }


    void SQLiteKeyStore::createTrigger(string_view triggerName,
                                       string_view triggerSuffix,
                                       string_view operation,
                                       string when,
                                       string_view statements)
    {
        if (hasPrefix(when, "WHERE"))
            when.replace(0, 5, "WHEN");
        string sql = CONCAT("CREATE TRIGGER \"" << triggerName << "::" << triggerSuffix  << "\" "
                            << operation << " ON kv_" << name() << ' ' << when << ' '
                            << " BEGIN " << statements << "; END");
        LogTo(QueryLog, "    ...for index: %s", sql.c_str());
        db().exec(sql);
    }


    vector<alloc_slice> SQLiteKeyStore::withDocBodies(const vector<slice> &docIDs,
                                                      WithDocBodyCallback callback)
    {
        if (docIDs.empty())
            return {};

        unordered_map<slice,size_t> docIndices; // maps docID -> index in docIDs[]
        docIndices.reserve(docIDs.size());

        // Construct SQL query with a big "IN (...)" clause for all the docIDs:
        stringstream sql;
        sql << "SELECT key, fl_callback(key, body, sequence, flags, ?) FROM kv_" << name()
            << " WHERE key IN ('";
        unsigned n = 0;
        for (slice docID : docIDs) {
            docIndices.insert({docID, n});
            if (n++ > 0)
                sql << "','";
            if (docID.findByte('\'')) {
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
        alloc_slice empty(size_t(0));
        vector<alloc_slice> results(docIDs.size());
        while (stmt.executeStep()) {
            slice docID = columnAsSlice(stmt.getColumn(0));
            slice value = textColumnAsSlice(stmt.getColumn(1));
            size_t i = docIndices[docID];
            //Log("    -- %zu: %.*s --> '%.*s'", i, SPLAT(docID), SPLAT(revs));
            if (value.size == 0 && value.buf != 0)
                results[i] = empty;     // reuse one empty slice instead of creating one per row
            else
                results[i] = alloc_slice(value);
        }
        return results;
    }


#pragma mark - EXPIRATION:


    // Returns true if the KeyStore's table has had the 'expiration' column added to it.
    bool SQLiteKeyStore::mayHaveExpiration() {
        if (!_hasExpirationColumn) {
            string sql;
            string tableName = "kv_" + name();
            db().getSchema(tableName, "table", tableName, sql);
            if (sql.find("expiration") != string::npos)
                _hasExpirationColumn = true;
        }
        return _hasExpirationColumn;
    }


    // Adds the 'expiration' column to the table.
    void SQLiteKeyStore::addExpiration() {
        if (mayHaveExpiration())
            return;
        db()._logVerbose("Adding the `expiration` column & index to kv_%s", name().c_str());
        db().execWithLock(subst(
                    "ALTER TABLE kv_@ ADD COLUMN expiration INTEGER; "
                    "CREATE INDEX kv_@_expiration ON kv_@ (expiration) WHERE expiration not null"));
        _hasExpirationColumn = true;
        _uncommittedExpirationColumn = true;
    }


    bool SQLiteKeyStore::setExpiration(slice key, expiration_t expTime) {
        Assert(expTime >= 0, "Invalid (negative) expiration time");
        addExpiration();
        compile(_setExpStmt, "UPDATE kv_@ SET expiration=? WHERE key=?");
        UsingStatement u(*_setExpStmt);
        if (expTime > 0)
            _setExpStmt->bind(1, (long long)expTime);
        else
            _setExpStmt->bind(1); // null
        _setExpStmt->bindNoCopy(2, (const char*)key.buf, (int)key.size);
        bool ok = _setExpStmt->exec() > 0;
        if (ok)
            db()._logVerbose("SQLiteKeyStore(%s) set expiration of '%.*s' to %" PRId64,
                            _name.c_str(), SPLAT(key), expTime);
        return ok;
    }


    expiration_t SQLiteKeyStore::getExpiration(slice key) {
        if (!mayHaveExpiration())
            return 0;
        compile(_getExpStmt, "SELECT expiration FROM kv_@ WHERE key=?");
        UsingStatement u(*_getExpStmt);
        _getExpStmt->bindNoCopy(1, (const char*)key.buf, (int)key.size);
        if (!_getExpStmt->executeStep())
            return 0;
        return _getExpStmt->getColumn(0);
    }


    expiration_t SQLiteKeyStore::nextExpiration() {
        expiration_t next = 0;
        if (mayHaveExpiration()) {
            compile(_nextExpStmt, "SELECT min(expiration) FROM kv_@");
            UsingStatement u(*_nextExpStmt);
            if (!_nextExpStmt->executeStep())
                return 0;
            next = _nextExpStmt->getColumn(0);
        }
        db()._logVerbose("Next expiration time is %" PRId64, next);
        return next;
    }


    unsigned SQLiteKeyStore::expireRecords(ExpirationCallback callback) {
        if (!mayHaveExpiration())
            return 0;
        expiration_t t = now();
        unsigned expired = 0;
        bool none = false;
        if (callback) {
            compile(_findExpStmt, "SELECT key FROM kv_@ WHERE expiration <= ?");
            UsingStatement u(*_findExpStmt);
            _findExpStmt->bind(1, (long long)t);
            none = true;
            while (_findExpStmt->executeStep()) {
                none = false;
                callback(columnAsSlice(_findExpStmt->getColumn(0)));
            }
        }
        if (!none) {
            expired = db().exec(format("DELETE FROM kv_%s WHERE expiration <= %" PRId64,
                                       name().c_str(), t));
        }
        db()._logInfo("Purged %u expired documents", expired);
        return expired;
    }

}
