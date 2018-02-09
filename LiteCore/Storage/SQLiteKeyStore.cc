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
#include "Fleece.hh"
#include <sstream>

using namespace std;
using namespace fleece;

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
        if (!db.keyStoreExists(name)) {
            // Here's the table schema. The body comes last because it may be very large, and it's
            // more efficient in SQLite to keep large columns at the end of a row.
            // Create the sequence and flags columns regardless of options, otherwise it's too
            // complicated to customize all the SQL queries to conditionally use them...
            db.execWithLock(subst("CREATE TABLE IF NOT EXISTS kv_@ ("
                          "  key TEXT PRIMARY KEY,"
                          "  sequence INTEGER,"
                          "  flags INTEGER DEFAULT 0,"
                          "  version BLOB,"
                          "  body BLOB)"));
        }
    }


    void SQLiteKeyStore::close() {
        // If statements are left open, closing the database will fail with a "db busy" error...
        _recCountStmt.reset();
        _getByKeyStmt.reset();
        _getMetaByKeyStmt.reset();
        _getBySeqStmt.reset();
        _getByOffStmt.reset();
        _getMetaBySeqStmt.reset();
        _setStmt.reset();
        _insertStmt.reset();
        _replaceStmt.reset();
        _delByKeyStmt.reset();
        _delBySeqStmt.reset();
        _delByBothStmt.reset();
        _backupStmt.reset();
        _setFlagStmt.reset();
        KeyStore::close();
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
            return new SQLite::Statement(db(), sql);
        } catch (const SQLite::Exception &x) {
            Warn("SQLite error compiling statement \"%s\": %s", sql.c_str(), x.what());
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
            const_cast<SQLiteKeyStore*>(this)->_lastSequence = seq;
        return seq;
    }

    
    void SQLiteKeyStore::setLastSequence(sequence_t seq) {
        if (_capabilities.sequences) {
            _lastSequence = seq;
            _lastSequenceChanged = true;
        }
    }


    void SQLiteKeyStore::transactionWillEnd(bool commit) {
        if (_lastSequenceChanged) {
            if (commit)
                db().setLastSequence(*this, _lastSequence);
            _lastSequenceChanged = false;
        }
        _lastSequence = -1;
    }


    /*static*/ slice SQLiteKeyStore::columnAsSlice(const SQLite::Column &col) {
        return slice(col.getBlob(), col.getBytes());
    }


    // OPT: Would be nice to avoid copying key/vers/body here; this would require Record to
    // know that the pointers are ephemeral, and create copies if they're accessed as
    // alloc_slice (not just slice).


    // Gets flags from col 1, version from col 3, and body (or its length) from col 4
    /*static*/ void SQLiteKeyStore::setRecordMetaAndBody(Record &rec,
                                                         SQLite::Statement &stmt,
                                                         ContentOptions options)
    {
        rec.setExists();
        rec.setFlags((DocumentFlags)(int)stmt.getColumn(1));
        rec.setVersion(columnAsSlice(stmt.getColumn(3)));
        if (options & kMetaOnly)
            rec.setUnloadedBodySize((ssize_t)stmt.getColumn(4));
        else
            rec.setBody(columnAsSlice(stmt.getColumn(4)));
    }
    

    bool SQLiteKeyStore::read(Record &rec, ContentOptions options) const {
        auto &stmt = (options & kMetaOnly)
            ? compile(_getMetaByKeyStmt,
                      "SELECT sequence, flags, 0, version, length(body) FROM kv_@ WHERE key=?")
            : compile(_getByKeyStmt,
                      "SELECT sequence, flags, 0, version, body FROM kv_@ WHERE key=?");
        stmt.bindNoCopy(1, (const char*)rec.key().buf, (int)rec.key().size);
        UsingStatement u(stmt);
        if (!stmt.executeStep())
            return false;

        sequence_t seq = (int64_t)stmt.getColumn(0);
        rec.updateSequence(seq);
        setRecordMetaAndBody(rec, stmt, options);
        return true;
    }


    Record SQLiteKeyStore::get(sequence_t seq /*, ContentOptions options*/) const {
        constexpr ContentOptions options = kDefaultContent;  // this used to be a param but not used
        if (!_capabilities.sequences)
            error::_throw(error::NoSequences);
        Record rec;
        auto &stmt = (options & kMetaOnly)
            ? compile(_getMetaBySeqStmt,
                      "SELECT 0, flags, key, version, length(body) FROM kv_@ WHERE sequence=?")
            : compile(_getBySeqStmt,
                      "SELECT 0, flags, key, version, body FROM kv_@ WHERE sequence=?");
        UsingStatement u(stmt);
        stmt.bind(1, (long long)seq);
        if (stmt.executeStep()) {
            rec.setKey(columnAsSlice(stmt.getColumn(2)));
            rec.updateSequence(seq);
            setRecordMetaAndBody(rec, stmt, options);
        }
        return rec;
    }


    sequence_t SQLiteKeyStore::set(slice key, slice vers, slice body, DocumentFlags flags,
                                   Transaction&,
                                   const sequence_t *replacingSequence,
                                   bool newSequence)
    {
        SQLite::Statement *stmt;
        if (replacingSequence == nullptr) {
            // Default:
            LogVerbose(DBLog, "KeyStore(%s) set %.*s", name().c_str(), SPLAT(key));
            compile(_setStmt,
                    "INSERT OR REPLACE INTO kv_@ (version, body, flags, sequence, key)"
                    " VALUES (?, ?, ?, ?, ?)");
            stmt = _setStmt.get();
        } else if (*replacingSequence == 0) {
            // Insert only:
            LogVerbose(DBLog, "KeyStore(%s) insert %.*s", name().c_str(), SPLAT(key));
            compile(_insertStmt,
                    "INSERT OR IGNORE INTO kv_@ (version, body, flags, sequence, key)"
                    " VALUES (?, ?, ?, ?, ?)");
            stmt = _insertStmt.get();
        } else {
            // Replace only:
            Assert(_capabilities.sequences);
            LogVerbose(DBLog, "KeyStore(%s) update %.*s", name().c_str(), SPLAT(key));
            compile(_replaceStmt,
                    "UPDATE kv_@ SET version=?, body=?, flags=?, sequence=?"
                    " WHERE key=? AND sequence=?");
            stmt = _replaceStmt.get();
            stmt->bind(6, (long long)*replacingSequence);
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
        LogVerbose(DBLog, "SQLiteKeyStore(%s) del key '%.*s' seq %llu",
                   _name.c_str(), SPLAT(key), seq);
        if (seq) {
            stmt = &compile(_delByBothStmt, "DELETE FROM kv_@ WHERE key=? AND sequence=?");
            stmt->bind(2, (long long)seq);
        } else {
            stmt = &compile(_delByKeyStmt, "DELETE FROM kv_@ WHERE key=?");
        }
        stmt->bindNoCopy(1, (const char*)key.buf, (int)key.size);
        UsingStatement u(*stmt);
        return stmt->exec() > 0;
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


    void SQLiteKeyStore::createTrigger(const string &triggerName,
                                       const char *triggerSuffix,
                                       const char *operation,
                                       const string &statements)
    {
        db().exec(CONCAT("CREATE TRIGGER \"" << triggerName << "::" << triggerSuffix
                            << "\" AFTER " << operation << " ON kv_" << name()
                            << " BEGIN " << statements << "; END"));
    }


    void SQLiteKeyStore::dropTrigger(const string &name, const char *suffix) {
        db().exec(CONCAT("DROP TRIGGER IF EXISTS \"" << name << "::" << suffix << "\""));
    }

}
