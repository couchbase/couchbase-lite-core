//
//  SQLiteKeyStore.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "QueryParser.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "Error.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sstream>

using namespace std;

namespace litecore {


    vector<string> SQLiteDataFile::allKeyStoreNames() {
        checkOpen();
        vector<string> names;
        SQLite::Statement allStores(*_sqlDb, string("SELECT substr(name,4) FROM sqlite_master"
                                                    " WHERE type='table' AND name GLOB 'kv_*'"));
        while (allStores.executeStep()) {
            names.push_back(allStores.getColumn(0));
        }
        return names;
    }


    bool SQLiteDataFile::keyStoreExists(const string &name) {
        checkOpen();
        SQLite::Statement storeExists(*_sqlDb, string("SELECT * FROM sqlite_master"
                                                      " WHERE type='table' AND name=?"));
        storeExists.bind(1, string("kv_") + name);
        bool exists = storeExists.executeStep();
        storeExists.reset();
        return exists;
    }


    SQLiteKeyStore::SQLiteKeyStore(SQLiteDataFile &db, const string &name, KeyStore::Capabilities capabilities)
    :KeyStore(db, name, capabilities)
    {
        if (!db.keyStoreExists(name)) {
            // Create the sequence and deleted columns regardless of options, otherwise it's too
            // complicated to customize all the SQL queries to conditionally use them...
            db.exec(subst("CREATE TABLE IF NOT EXISTS kv_@ (key BLOB PRIMARY KEY, meta BLOB, "
                          "body BLOB, sequence INTEGER, deleted INTEGER DEFAULT 0)"));
            if (capabilities.getByOffset) {
                // shadow table for overwritten records
                db.exec(subst("CREATE TABLE IF NOT EXISTS kvold_@ ("
                              "sequence INTEGER PRIMARY KEY, key BLOB, meta BLOB, body BLOB)"));
                db.exec("PRAGMA recursive_triggers = 1");
                db.exec(subst("CREATE TRIGGER backup_@ BEFORE DELETE ON kv_@ BEGIN "
                              "  INSERT INTO kvold_@ (sequence, key, meta, body) "
                              "    VALUES (OLD.sequence, OLD.key, OLD.meta, OLD.body); END"));
            }
        }
    }


    void SQLiteKeyStore::close() {
        _recCountStmt.reset();
        _getByKeyStmt.reset();
        _getMetaByKeyStmt.reset();
        _getBySeqStmt.reset();
        _getByOffStmt.reset();
        _getMetaBySeqStmt.reset();
        _setStmt.reset();
        _delByKeyStmt.reset();
        _delBySeqStmt.reset();
        _backupStmt.reset();
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
        return new SQLite::Statement(db(), sql);
    }


    SQLite::Statement& SQLiteKeyStore::compile(const unique_ptr<SQLite::Statement>& ref,
                                               const char *sqlTemplate) const
    {
        if (ref != nullptr) {
            db().checkOpen();
            ref->reset();  // prepare statement to be run again
            return *ref.get();
        } else {
            return db().compile(ref, subst(sqlTemplate).c_str());
        }
    }


    uint64_t SQLiteKeyStore::recordCount() const {
        if (!_recCountStmt) {
            stringstream sql;
            sql << "SELECT count(*) FROM kv_" << _name;
            if (_capabilities.softDeletes)
                sql << " WHERE deleted!=1";
            compile(_recCountStmt, sql.str().c_str());
        }
        UsingStatement u(_recCountStmt);
        if (_recCountStmt->executeStep()) {
            auto count = (int64_t)_recCountStmt->getColumn(0);
            return count;
        }
        return 0;
    }


    sequence SQLiteKeyStore::lastSequence() const {
        if (_lastSequence >= 0)
            return _lastSequence;
        sequence seq = db().lastSequence(_name);
        if (db().inTransaction())
            const_cast<SQLiteKeyStore*>(this)->_lastSequence = seq;
        return seq;
    }

    
    void SQLiteKeyStore::setLastSequence(sequence seq) {
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


    // OPT: Would be nice to avoid copying key/meta/body here; this would require Record to
    // know that the pointers are ephemeral, and create copies if they're accessed as
    // alloc_slice (not just slice).


    // Gets meta from column 3, and body (or its length) from column 4
    /*static*/ void SQLiteKeyStore::setDocMetaAndBody(Record &rec,
                                                      SQLite::Statement &stmt,
                                                      ContentOptions options)
    {
        rec.setMeta(columnAsSlice(stmt.getColumn(3)));
        if (options & kMetaOnly)
            rec.setUnloadedBodySize((ssize_t)stmt.getColumn(4));
        else
            rec.setBody(columnAsSlice(stmt.getColumn(4)));
    }
    

    bool SQLiteKeyStore::read(Record &rec, ContentOptions options) const {
        auto &stmt = (options & kMetaOnly)
            ? compile(_getMetaByKeyStmt,
                      "SELECT sequence, deleted, 0, meta, length(body) FROM kv_@ WHERE key=?")
            : compile(_getByKeyStmt,
                      "SELECT sequence, deleted, 0, meta, body FROM kv_@ WHERE key=?");
        stmt.bindNoCopy(1, rec.key().buf, (int)rec.key().size);
        UsingStatement u(stmt);
        if (!stmt.executeStep())
            return false;

        sequence seq = (int64_t)stmt.getColumn(0);
        uint64_t offset = _capabilities.getByOffset ? seq : 0;
        bool deleted = (int)stmt.getColumn(1);
        updateDoc(rec, seq, offset, deleted);
        setDocMetaAndBody(rec, stmt, options);
        return !rec.deleted();
    }


    Record SQLiteKeyStore::get(sequence seq, ContentOptions options) const {
        if (!_capabilities.sequences)
            error::_throw(error::NoSequences);
        Record rec;
        auto &stmt = (options & kMetaOnly)
            ? compile(_getMetaBySeqStmt,
                          "SELECT 0, deleted, key, meta, length(body) FROM kv_@ WHERE sequence=?")
            : compile(_getBySeqStmt,
                           "SELECT 0, deleted, key, meta, body FROM kv_@ WHERE sequence=?");
        UsingStatement u(stmt);
        stmt.bind(1, (int64_t)seq);
        if (stmt.executeStep()) {
            uint64_t offset = _capabilities.getByOffset ? seq : 0;
            bool deleted = (int)stmt.getColumn(1);
            updateDoc(rec, seq, offset, deleted);
            rec.setKey(columnAsSlice(stmt.getColumn(2)));
            setDocMetaAndBody(rec, stmt, options);
        }
        return rec;
    }


    Record SQLiteKeyStore::getByOffsetNoErrors(uint64_t offset, sequence seq) const {
        Assert(offset == seq);
        Record rec;
        if (!_capabilities.getByOffset)
            return rec;

        auto &stmt = compile(_getByOffStmt, "SELECT key, meta, body FROM kvold_@ WHERE sequence=?");
        UsingStatement u(stmt);
        stmt.bind(1, (int64_t)seq);
        if (stmt.executeStep()) {
            updateDoc(rec, seq, seq);
            rec.setKey(columnAsSlice(stmt.getColumn(0)));
            rec.setMeta(columnAsSlice(stmt.getColumn(1)));
            rec.setBody(columnAsSlice(stmt.getColumn(2)));
            return rec;
        } else {
            // Maybe the sequence is still current...
            return get(seq, kDefaultContent);
        }
    }


    KeyStore::setResult SQLiteKeyStore::set(slice key, slice meta, slice body, Transaction&) {
        LogTo(DBLog, "KeyStore(%s) set %s", name().c_str(), logSlice(key));
        compile(_setStmt,
                "INSERT OR REPLACE INTO kv_@ (key, meta, body, sequence, deleted) VALUES (?, ?, ?, ?, 0)");
        _setStmt->bindNoCopy(1, key.buf, (int)key.size);
        _setStmt->bindNoCopy(2, meta.buf, (int)meta.size);
        _setStmt->bindNoCopy(3, body.buf, (int)body.size);

        sequence seq = 0;
        if (_capabilities.sequences) {
            seq = lastSequence() + 1;
            _setStmt->bind(4, (int64_t)seq);
        } else {
            _setStmt->bind(4);
        }
        UsingStatement u(_setStmt);
        _setStmt->exec();
        setLastSequence(seq);
        return {seq, (_capabilities.getByOffset ? seq : 0)};
    }


    bool SQLiteKeyStore::_del(slice key, sequence delSeq, Transaction&) {
        auto& stmt = delSeq ? _delBySeqStmt : _delByKeyStmt;
        if (!stmt) {
            stringstream sql;
            if (_capabilities.softDeletes) {
                sql << "UPDATE kv_@ SET deleted=1, meta=null, body=null";
                if (_capabilities.sequences)
                    sql << ", sequence=? ";
            } else {
                sql << "DELETE FROM kv_@";
            }
            sql << (delSeq ? " WHERE sequence=?" : " WHERE key=?");
            compile(stmt, sql.str().c_str());
        }

        sequence newSeq = 0;
        int param = 1;
        if (_capabilities.softDeletes && _capabilities.sequences) {
            newSeq = lastSequence() + 1;
            stmt->bind(param++, (int64_t)newSeq);
        }
        if (delSeq)
            stmt->bind(param++, (int64_t)delSeq);
        else
            stmt->bindNoCopy(param++, key.buf, (int)key.size);

        UsingStatement u(stmt);
        bool ok = stmt->exec() > 0;
        if (ok && newSeq > 0)
            setLastSequence(newSeq);
        return ok;
    }


    void SQLiteKeyStore::erase() {
        Transaction t(db());
        db()._sqlDb->exec(string("DELETE FROM kv_"+name()));
        setLastSequence(0);
        t.commit();
    }


    // Writes a SQL string containing a unique name for the index with the given path.
    void SQLiteKeyStore::writeSQLIndexName(const string &propertyPath, stringstream &sql) {
        sql << "'" << name() << "::" << propertyPath << "'";
    }


    void SQLiteKeyStore::createIndex(const string &propertyExpression) {
        stringstream sql;
        sql << "CREATE INDEX IF NOT EXISTS ";
        writeSQLIndexName(propertyExpression, sql);
        sql << " ON kv_" << name() << " (";
        sql << QueryParser::propertyGetter(propertyExpression);
        sql << ")";
        // TODO: Add 'WHERE' clause for use with SQLite 3.15+
        db()._sqlDb->exec(sql.str());
    }


    void SQLiteKeyStore::deleteIndex(const string &propertyPath) {
        stringstream sql;
        sql << "DROP INDEX ";
        writeSQLIndexName(propertyPath, sql);
        db()._sqlDb->exec(sql.str());

    }

}
