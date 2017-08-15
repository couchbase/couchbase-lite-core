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
#include "Fleece.hh"
#include <sstream>
#include <iostream>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"
}

using namespace std;
using namespace fleece;

namespace litecore {


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
            ref->reset();  // prepare statement to be run again
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


    Record SQLiteKeyStore::get(sequence_t seq, ContentOptions options) const {
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
                                   Transaction&, const sequence_t *replacingSequence) {
        SQLite::Statement *stmt;
        if (replacingSequence == nullptr) {
            // Default:
            LogVerbose(DBLog, "KeyStore(%s) set %s", name().c_str(), logSlice(key));
            compile(_setStmt,
                    "INSERT OR REPLACE INTO kv_@ (version, body, flags, sequence, key)"
                    " VALUES (?, ?, ?, ?, ?)");
            stmt = _setStmt.get();
        } else if (*replacingSequence == 0) {
            // Insert only:
            LogVerbose(DBLog, "KeyStore(%s) insert %s", name().c_str(), logSlice(key));
            compile(_insertStmt,
                    "INSERT OR IGNORE INTO kv_@ (version, body, flags, sequence, key)"
                    " VALUES (?, ?, ?, ?, ?)");
            stmt = _insertStmt.get();
        } else {
            // Replace only:
            Assert(_capabilities.sequences);
            LogVerbose(DBLog, "KeyStore(%s) update %s", name().c_str(), logSlice(key));
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
            seq = lastSequence() + 1;
            stmt->bind(4, (long long)seq);
        } else {
            stmt->bind(4); // null
        }

        UsingStatement u(*stmt);
        if (stmt->exec() == 0)
            return 0;               // condition wasn't met
        setLastSequence(seq);
        return seq;
    }


    bool SQLiteKeyStore::_del(slice key, sequence_t seq, Transaction&) {
        SQLite::Statement *stmt;
        if (key) {
            if (seq) {
                stmt = &compile(_delByBothStmt, "DELETE FROM kv_@ WHERE key=? AND sequence=?");
                stmt->bind(2, (long long)seq);
            } else {
                stmt = &compile(_delByKeyStmt, "DELETE FROM kv_@ WHERE key=?");
            }
            stmt->bindNoCopy(1, (const char*)key.buf, (int)key.size);
        } else {
            stmt = &compile(_delBySeqStmt, "DELETE FROM kv_@ WHERE sequence=?");
            stmt->bind(1, (long long)seq);
        }
        UsingStatement u(*stmt);
        return stmt->exec() > 0;
    }


    bool SQLiteKeyStore::setDocumentFlag(slice key, sequence_t sequence, DocumentFlags flags) {
        compile(_setFlagStmt, "UPDATE kv_@ SET flags=(flags | ?) WHERE key=? AND sequence=?");
        UsingStatement u(*_setFlagStmt);
        _setFlagStmt->bind      (1, (unsigned)flags);
        _setFlagStmt->bindNoCopy(2, (const char*)key.buf, (int)key.size);
        _setFlagStmt->bind      (3, (long long)sequence);
        return _setFlagStmt->exec() > 0;
    }


    void SQLiteKeyStore::erase() {
        Transaction t(db());
        db().exec(string("DELETE FROM kv_"+name()));
        setLastSequence(0);
        t.commit();
    }


#pragma mark - INDEXES:


    // Returns a unique name for the FTS table that indexes the given property path.
    string SQLiteKeyStore::SQLIndexName(const Array *expression, IndexType type, bool quoted) {
        stringstream sql;
        if (quoted)
            sql << '"';
        QueryParser qp(tableName());
        sql << (type == kFullTextIndex ? qp.FTSIndexName(expression) : qp.indexName(expression));
        if (quoted)
            sql << '"';
        return sql.str();
    }


    // Parses the JSON index-spec expression into an Array:
    static pair<alloc_slice, const Array*> parseIndexExpr(slice expression,
                                                          KeyStore::IndexType type)
    {
        alloc_slice expressionFleece;
        const Array *params = nullptr;
        try {
            expressionFleece = JSONConverter::convertJSON(expression);
            auto f = Value::fromTrustedData(expressionFleece);
            if (f)
                params = f->asArray();
        } catch (const FleeceException &) { }
        if (!params || params->count() == 0)
            error::_throw(error::InvalidQuery);

        if (type == KeyStore::kFullTextIndex) {
            // Full-text index can only have one key, so use that:
            if (params->count() != 1)
                error::_throw(error::InvalidQuery);
            params = params->get(0)->asArray();
            if (!params)
                error::_throw(error::InvalidQuery);
        }

        return {expressionFleece, params};
    }


    void SQLiteKeyStore::createIndex(slice expression,
                                     IndexType type,
                                     const IndexOptions *options) {
        alloc_slice expressionFleece;
        const Array *params;
        tie(expressionFleece, params) = parseIndexExpr(expression, type);

        Transaction t(db());
        switch (type) {
            case  kValueIndex: {
                QueryParser qp(tableName());
                qp.writeCreateIndex(params);
                db().exec(qp.SQL(), LogLevel::Info);
                break;
            }
            case kFullTextIndex: {
                // Create the FTS4 virtual table: ( https://www.sqlite.org/fts3.html )
                auto tableName = SQLIndexName(params, type);
                if (db().tableExists(tableName))
                    return;
                stringstream sql;
                sql << "CREATE VIRTUAL TABLE \"" << tableName << "\" USING fts4(text, tokenize=unicodesn";
                if (options) {
                    if (options->stemmer) {
                        if (unicodesn_isSupportedStemmer(options->stemmer)) {
                            sql << " \"stemmer=" << options->stemmer << "\"";
                        } else {
                            Warn("FTS does not support language code '%s'; ignoring it",
                                 options->stemmer);
                        }
                    }
                    if (options->ignoreDiacritics) {
                        sql << " \"remove_diacritics=1\"";
                    }
                }
                sql << ")";
                db().exec(sql.str(), LogLevel::Info);

                // Index existing records:
                db().exec("INSERT INTO \"" + tableName + "\" (rowid, text) SELECT sequence, " + QueryParser::expressionSQL(params, "body") + " FROM kv_" + name());

                // Set up triggers to keep the FTS5 table up to date:
                string ins = "INSERT INTO \"" + tableName + "\" (rowid, text) VALUES (new.sequence, " + QueryParser::expressionSQL(params, "new.body") + "); ";
                string del = "DELETE FROM \"" + tableName + "\" WHERE rowid = old.sequence; ";

                db().exec(string("CREATE TRIGGER \"") + tableName + "::ins\" AFTER INSERT ON kv_" + name() + " BEGIN " + ins + " END");
                db().exec(string("CREATE TRIGGER \"") + tableName + "::del\" AFTER DELETE ON kv_" + name() + " BEGIN " + del + " END");
                db().exec(string("CREATE TRIGGER \"") + tableName + "::upd\" AFTER UPDATE ON kv_" + name() + " BEGIN " + del + ins + " END");
                break;
            }
            default:
                error::_throw(error::Unimplemented);
        }
        t.commit();
    }


    void SQLiteKeyStore::deleteIndex(slice expression, IndexType type) {
        alloc_slice expressionFleece;
        const Array *params;
        tie(expressionFleece, params) = parseIndexExpr(expression, type);
        string indexName = SQLIndexName(params, type, true);

        Transaction t(db());
        switch (type) {
            case  kValueIndex:
                db().exec(string("DROP INDEX ") + indexName, LogLevel::Info);
                break;
            case kFullTextIndex: {
                db().exec(string("DROP VIRTUAL TABLE ") + indexName, LogLevel::Info);
                // TODO: Do I have to explicitly delete the triggers too?
                break;
            }
            default:
                error::_throw(error::Unimplemented);
        }
        t.commit();
    }


    bool SQLiteKeyStore::hasIndex(slice expression, IndexType type) {
        alloc_slice expressionFleece;
        const Array *params;
        tie(expressionFleece, params) = parseIndexExpr(expression, type);
        string indexName = SQLIndexName(params, type);

        switch (type) {
            case kFullTextIndex: {
                return db().tableExists(indexName);
                break;
            }
            default:
                error::_throw(error::Unimplemented);
        }
    }


    void SQLiteKeyStore::createSequenceIndex() {
        if (!_createdSeqIndex) {
            if (!_capabilities.sequences)
                error::_throw(error::NoSequences);
            db().execWithLock(string("CREATE UNIQUE INDEX IF NOT EXISTS kv_"+name()+"_seqs"
                                     " ON kv_"+name()+" (sequence)"));
            _createdSeqIndex = true;
        }
    }

}
