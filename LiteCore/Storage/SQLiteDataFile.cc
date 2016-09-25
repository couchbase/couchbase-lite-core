//
//  SQLiteDatabase.cc
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
#include "Document.hh"
#include "DocEnumerator.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>
#include <sstream>

using namespace std;

namespace litecore {


    SQLiteDataFile::Factory& SQLiteDataFile::factory() {
        static SQLiteDataFile::Factory s;
        return s;
    }
    

    SQLiteDataFile* SQLiteDataFile::Factory::openFile(const FilePath &path, const Options *options) {
        return new SQLiteDataFile(path, options);
    }


    bool SQLiteDataFile::Factory::deleteFile(const FilePath &path, const Options*) {
        return path.del() | path.withExtension("shm").del() | path.withExtension("wal").del();
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
        int sqlFlags = options().writeable ? SQLite::OPEN_READWRITE : SQLite::OPEN_READONLY;
        if (options().create)
            sqlFlags |= SQLite::OPEN_CREATE;
        _sqlDb.reset(new SQLite::Database(filePath().path().c_str(), sqlFlags));

        if (!decrypt())
            error::_throw(error::UnsupportedEncryption);

        withFileLock([this]{
            _sqlDb->exec("PRAGMA mmap_size=50000000");
            _sqlDb->exec("PRAGMA journal_mode=WAL");
            _sqlDb->exec("CREATE TABLE IF NOT EXISTS kvmeta (name TEXT PRIMARY KEY,"
                         " lastSeq INTEGER DEFAULT 0) WITHOUT ROWID");
            (void)defaultKeyStore();    // make sure its table is created
        });
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


    bool SQLiteDataFile::encryptionEnabled() {
        // Check whether encryption is available:
        if (sqlite3_compileoption_used("SQLITE_HAS_CODEC") == 0)
            return false;
        // Try to determine whether we're using SQLCipher or the SQLite Encryption Extension,
        // by calling a SQLCipher-specific pragma that returns a number:
        SQLite::Statement s(*_sqlDb, "PRAGMA cipher_default_kdf_iter");
        if (!s.executeStep()) {
            // Oops, this isn't SQLCipher, so we can't use encryption. (SEE requires us to call
            // another pragma to enable encryption, which takes a license key we don't know.)
            return false;
        }
        return true;
    }


    bool SQLiteDataFile::decrypt() {
        if (options().encryptionAlgorithm != kNoEncryption) {
            if (options().encryptionAlgorithm != kAES256 || !encryptionEnabled())
                return false;

            // Set the encryption key in SQLite:
            slice key = options().encryptionKey;
            if(key.buf == NULL || key.size != 32)
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
                if(newKey.buf == NULL || newKey.size != 32)
                    error::_throw(error::InvalidParameter);
                break;
            default:
                error::_throw(error::InvalidParameter);
        }

        if (!encryptionEnabled())
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
        CBFAssert(_transaction == nullptr);
        _transaction.reset( new SQLite::Transaction(*_sqlDb) );
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
            const_cast<unique_ptr<SQLite::Statement>&>(ref).reset(
                                                      new SQLite::Statement(*_sqlDb, sql));
        ref->reset();  // prepare statement to be run again
        return *ref.get();
    }


    sequence SQLiteDataFile::lastSequence(const string& keyStoreName) const {
        sequence seq = 0;
        compile(_getLastSeqStmt, "SELECT lastSeq FROM kvmeta WHERE name=?");
        _getLastSeqStmt->bindNoCopy(1, keyStoreName);
        if (_getLastSeqStmt->executeStep()) {
            seq = (int64_t)_getLastSeqStmt->getColumn(0);
            _getLastSeqStmt->reset();
        }
        return seq;
    }

    void SQLiteDataFile::setLastSequence(SQLiteKeyStore &store, sequence seq) {
        compile(_setLastSeqStmt,
                "INSERT OR REPLACE INTO kvmeta (name, lastSeq) VALUES (?, ?)");
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


#pragma mark - KEY-STORE:


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
                // shadow table for overwritten docs
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
        _docCountStmt.reset();
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


    uint64_t SQLiteKeyStore::documentCount() const {
        if (_docCountStmt) {
            _docCountStmt->reset();
        } else {
            stringstream sql;
            sql << "SELECT count(*) FROM kv_" << _name;
            if (_capabilities.softDeletes)
                sql << " WHERE deleted!=1";
            compile(_docCountStmt, sql.str().c_str());
        }
        if (_docCountStmt->executeStep()) {
            auto count = (int64_t)_docCountStmt->getColumn(0);
            _docCountStmt->reset();
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


    static slice columnAsSlice(const SQLite::Column &col) {
        return slice(col.getBlob(), col.getBytes());
    }


    // OPT: Would be nice to avoid copying key/meta/body here; this would require Document to
    // know that the pointers are ephemeral, and create copies if they're accessed as
    // alloc_slice (not just slice).


    // Gets meta from column 3, and body (or its length) from column 4
    static void setDocMetaAndBody(Document &doc,
                                  SQLite::Statement &stmt,
                                  ContentOptions options)
    {
        doc.setMeta(columnAsSlice(stmt.getColumn(3)));
        if (options & kMetaOnly)
            doc.setUnloadedBodySize((ssize_t)stmt.getColumn(4));
        else
            doc.setBody(columnAsSlice(stmt.getColumn(4)));
    }
    

    bool SQLiteKeyStore::read(Document &doc, ContentOptions options) const {
        auto &stmt = (options & kMetaOnly)
            ? compile(_getMetaByKeyStmt,
                      "SELECT sequence, deleted, 0, meta, length(body) FROM kv_@ WHERE key=?")
            : compile(_getByKeyStmt,
                      "SELECT sequence, deleted, 0, meta, body FROM kv_@ WHERE key=?");
        stmt.bindNoCopy(1, doc.key().buf, (int)doc.key().size);
        if (!stmt.executeStep())
            return false;

        sequence seq = (int64_t)stmt.getColumn(0);
        uint64_t offset = _capabilities.getByOffset ? seq : 0;
        bool deleted = (int)stmt.getColumn(1);
        updateDoc(doc, seq, offset, deleted);
        setDocMetaAndBody(doc, stmt, options);
        stmt.reset();
        return !doc.deleted();
    }


    Document SQLiteKeyStore::get(sequence seq, ContentOptions options) const {
        if (!_capabilities.sequences)
            error::_throw(error::NoSequences);
        Document doc;
        auto &stmt = (options & kMetaOnly)
            ? compile(_getMetaBySeqStmt,
                          "SELECT 0, deleted, key, meta, length(body) FROM kv_@ WHERE sequence=?")
            : compile(_getBySeqStmt,
                           "SELECT 0, deleted, key, meta, body FROM kv_@ WHERE sequence=?");
        stmt.bind(1, (int64_t)seq);
        if (stmt.executeStep()) {
            uint64_t offset = _capabilities.getByOffset ? seq : 0;
            bool deleted = (int)stmt.getColumn(1);
            updateDoc(doc, seq, offset, deleted);
            doc.setKey(columnAsSlice(stmt.getColumn(2)));
            setDocMetaAndBody(doc, stmt, options);
            stmt.reset();
        }
        return doc;
    }


    Document SQLiteKeyStore::getByOffsetNoErrors(uint64_t offset, sequence seq) const {
        CBFAssert(offset == seq);
        Document doc;
        if (!_capabilities.getByOffset)
            return doc;

        auto &stmt = compile(_getByOffStmt, "SELECT key, meta, body FROM kvold_@ WHERE sequence=?");
        stmt.bind(1, (int64_t)seq);
        if (stmt.executeStep()) {
            updateDoc(doc, seq, seq);
            doc.setKey(columnAsSlice(stmt.getColumn(0)));
            doc.setMeta(columnAsSlice(stmt.getColumn(1)));
            doc.setBody(columnAsSlice(stmt.getColumn(2)));
            stmt.reset();
            return doc;
        } else {
            // Maybe the sequence is still current...
            return get(seq, kDefaultContent);
        }
    }


    KeyStore::setResult SQLiteKeyStore::set(slice key, slice meta, slice body, Transaction&) {
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

        bool ok = stmt->exec() > 0;
        if (ok && newSeq > 0)
            setLastSequence(newSeq);
        stmt->reset();
        return ok;
    }


    void SQLiteKeyStore::erase() {
        Transaction t(db());
        db()._sqlDb->exec(string("DELETE FROM kv_"+name()));
        setLastSequence(0);
        t.commit();
    }


#pragma mark - ITERATOR:


    class SQLiteIterator : public DocEnumerator::Impl {
    public:
        SQLiteIterator(SQLite::Statement *stmt, bool descending, ContentOptions content)
        :_stmt(stmt),
         _content(content)
        { }

        virtual bool next() override {
            return _stmt->executeStep();
        }

        virtual bool read(Document &doc) override {
            updateDoc(doc, (int64_t)_stmt->getColumn(0), 0, (int)_stmt->getColumn(1));
            doc.setKey(columnAsSlice(_stmt->getColumn(2)));
            setDocMetaAndBody(doc, *_stmt.get(), _content);
            return true;
        }

    private:
        unique_ptr<SQLite::Statement> _stmt;
        ContentOptions _content;
    };


    stringstream SQLiteKeyStore::selectFrom(const DocEnumerator::Options &options) {
        stringstream sql;
        sql << "SELECT sequence, deleted, key, meta";
        if (options.contentOptions & kMetaOnly)
            sql << ", length(body)";
        else
            sql << ", body";
        sql << " FROM kv_" << name();
        return sql;
    }

    void SQLiteKeyStore::writeSQLOptions(stringstream &sql, DocEnumerator::Options &options) {
        if (options.descending)
            sql << " DESC";
        if (options.limit < UINT_MAX)
            sql << " LIMIT " << options.limit;
        if (options.skip > 0) {
            if (options.limit == UINT_MAX)
                sql << " LIMIT -1";             // OFFSET has to have a LIMIT before it
            sql << " OFFSET " << options.skip;
            options.skip = 0;                   // tells DocEnumerator not to do skip on its own
        }
        options.limit = UINT_MAX;               // ditto for limit
    }


    // iterate by key:
    DocEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(slice minKey, slice maxKey,
                                                           DocEnumerator::Options &options)
    {
        stringstream sql = selectFrom(options);
        bool noDeleted = _capabilities.softDeletes && !options.includeDeleted;
        if (minKey.buf || maxKey.buf || noDeleted) {
            sql << " WHERE ";
            bool writeAnd = false;
            if (minKey.buf) {
                sql << (options.inclusiveMin() ? "key >= ?" : "key > ?");
                writeAnd = true;
            }
            if (maxKey.buf) {
                if (writeAnd) sql << " AND "; else writeAnd = true;
                sql << (options.inclusiveMax() ? "key <= ?" : "key < ?");
            }
            if (_capabilities.softDeletes && noDeleted) {
                if (writeAnd) sql << " AND "; //else writeAnd = true;
                sql << "deleted!=1";
            }
        }
        sql << " ORDER BY key";
        writeSQLOptions(sql, options);

        auto st = new SQLite::Statement(db(), sql.str());    //TODO: Cache a statement
        int param = 1;
        if (minKey.buf)
            st->bind(param++, minKey.buf, (int)minKey.size);
        if (maxKey.buf)
            st->bind(param++, maxKey.buf, (int)maxKey.size);
        return new SQLiteIterator(st, options.descending, options.contentOptions);
    }

    // iterate by sequence:
    DocEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(sequence min, sequence max,
                                                           DocEnumerator::Options &options)
    {
        if (!_capabilities.sequences)
            error::_throw(error::NoSequences);

        if (!_createdSeqIndex) {
            db().exec(string("CREATE UNIQUE INDEX IF NOT EXISTS kv_"+name()+"_seqs"
                                          " ON kv_"+name()+" (sequence)"));
            _createdSeqIndex = true;
        }

        stringstream sql = selectFrom(options);
        sql << (options.inclusiveMin() ? " WHERE sequence >= ?" : " WHERE sequence > ?");
        if (max < INT64_MAX)
            sql << (options.inclusiveMax() ? " AND sequence <= ?"   : " AND sequence < ?");
        if (_capabilities.softDeletes && !options.includeDeleted)
            sql << " AND deleted!=1";
        sql << " ORDER BY sequence";
        writeSQLOptions(sql, options);

        auto st = new SQLite::Statement(db(), sql.str());        // TODO: Cache a statement
        st->bind(1, (int64_t)min);
        if (max < INT64_MAX)
            st->bind(2, (int64_t)max);
        return new SQLiteIterator(st, options.descending, options.contentOptions);
    }

}
