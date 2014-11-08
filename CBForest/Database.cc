//
//  Database.cc
//  CBForest
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Database.hh"
#include "LogInternal.hh"
#include "option.h"           // forestdb internal header; for FDB_MAX_KEYLEN etc.
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>


namespace forestdb {

    static void defaultLogCallback(logLevel level, const char *message) {
        fprintf(stderr, "CBForest: %s\n", message);
    }

    logLevel LogLevel = kWarning;
    void (*LogCallback)(logLevel, const char *message) = &defaultLogCallback;

    void _Log(logLevel level, const char *message, ...) {
        if (LogLevel <= level && LogCallback != NULL) {
            va_list args;
            va_start(args, message);
            char *formatted = NULL;
            vasprintf(&formatted, message, args);
            va_end(args);
            LogCallback(level, formatted);
        }
    }

#pragma mark - FILE:

    class Database::File {
    public:
        static File* forPath(std::string path);
        File();
        std::mutex _transactionMutex;
        std::condition_variable _transactionCond;
        Transaction* _transaction;

        static std::unordered_map<std::string, File*> sFileMap;
        static std::mutex sMutex;
    };

    std::unordered_map<std::string, Database::File*> Database::File::sFileMap;
    std::mutex Database::File::sMutex;

    Database::File* Database::File::forPath(std::string path) {
        std::unique_lock<std::mutex> lock(sMutex);
        File* file = sFileMap[path];
        if (!file) {
            file = new File();
            sFileMap[path] = file;
        }
        return file;
    }

    Database::File::File()
    :_transaction(NULL)
    { }

#pragma mark - DATABASE:

    static void check(fdb_status status) {
        if (status != FDB_RESULT_SUCCESS) {
            WarnError("FORESTDB ERROR %d\n", status);
            throw error{status};
        }
    }

    DatabaseGetters::DatabaseGetters()
    :_handle(NULL)
    { }

    Database::Database(std::string path, openFlags flags, const config& cfg)
    :_file(File::forPath(path)), _openFlags(flags), _config(cfg)
    {
        check(::fdb_open(&_handle, path.c_str(), (config*)&cfg));
    }

    Database::~Database() {
        if (_handle)
            fdb_close(_handle);
    }

    DatabaseGetters::info DatabaseGetters::getInfo() const {
        info i;
        check(fdb_get_dbinfo(_handle, &i));
        return i;
    }

    std::string DatabaseGetters::filename() const {
        return std::string(this->getInfo().filename);
    }

    bool Database::isReadOnly() const {
        return (_openFlags & FDB_OPEN_FLAG_RDONLY) != 0;
    }

    void Database::deleted() {
        _handle = NULL;
    }

    
#pragma mark - GET:

    static bool checkGet(fdb_status status) {
        if (status == FDB_RESULT_KEY_NOT_FOUND)
            return false;
        check(status);
        return true;
    }

    Document DatabaseGetters::get(slice key, contentOptions options) const {
        Document doc(key);
        read(doc, options);
        return doc;
    }

    Document DatabaseGetters::get(sequence seq, contentOptions options) const {
        Document doc;
        doc._doc.seqnum = seq;
        if (options & kMetaOnly)
            check(fdb_get_metaonly_byseq(_handle, &doc._doc));
        else
            check(fdb_get_byseq(_handle, doc));
        return doc;
    }

    bool DatabaseGetters::read(Document& doc, contentOptions options) const {
        doc.clearMetaAndBody();
        if (options & kMetaOnly)
            return checkGet(fdb_get_metaonly(_handle, doc));
        else
            return checkGet(fdb_get(_handle, doc));
    }

    Document DatabaseGetters::getByOffset(uint64_t offset, sequence seq) {
        Document doc;
        doc._doc.offset = offset;
        doc._doc.seqnum = seq;
        checkGet(fdb_get_byoffset(_handle, doc));
        return doc;
    }


#pragma mark - DOCUMENTS:

    const size_t Document::kMaxKeyLength  = FDB_MAX_KEYLEN;
    const size_t Document::kMaxMetaLength = FDB_MAX_METALEN;
    const size_t Document::kMaxBodyLength = FDB_MAX_BODYLEN;

    Document::Document() {
        memset(&_doc, 0, sizeof(_doc));
    }

    Document::Document(const Document& doc) {
        memset(&_doc, 0, sizeof(_doc));
        setKey(doc.key());
        setMeta(doc.meta());
        setBody(doc.body());
        _doc.size_ondisk = doc.sizeOnDisk();
        _doc.seqnum = doc.sequence();
        _doc.offset = doc.offset();
        _doc.deleted = doc.deleted();
    }

    Document::Document(Document&& doc) {
        memcpy(&_doc, &doc._doc, sizeof(_doc));
        doc._doc.key = doc._doc.body = doc._doc.meta = NULL; // to prevent double-free
    }

    Document::Document(slice key) {
        memset(&_doc, 0, sizeof(_doc));
        setKey(key);
    }

    Document::~Document() {
        key().free();
        meta().free();
        body().free();
    }

    bool Document::valid() const {
        return _doc.key != NULL && _doc.keylen > 0 && _doc.keylen <= kMaxKeyLength
            && _doc.metalen <= kMaxMetaLength && !(_doc.metalen != 0 && _doc.meta == NULL)
            && _doc.bodylen <= kMaxBodyLength && !(_doc.bodylen != 0 && _doc.body == NULL);
    }

    void Document::clearMetaAndBody() {
        setMeta(slice::null);
        setBody(slice::null);
        _doc.seqnum = 0;
        _doc.offset = 0;
        _doc.deleted = false;
    }

    static inline void _assign(void* &buf, size_t &size, slice s) {
        ::free(buf);
        buf = (void*)s.copy().buf;
        size = s.size;
    }

    void Document::setKey(slice key)   {_assign(_doc.key,  _doc.keylen,  key);}
    void Document::setMeta(slice meta) {_assign(_doc.meta, _doc.metalen, meta);}
    void Document::setBody(slice body) {_assign(_doc.body, _doc.bodylen, body);}

    slice Document::resizeMeta(size_t newSize) {
        if (newSize != _doc.metalen) {
            void* newMeta = realloc(_doc.meta, newSize);
            if (!newMeta)
                throw std::bad_alloc();
            _doc.meta = newMeta;
            _doc.metalen = newSize;
        }
        return meta();
    }



#pragma mark - TRANSACTION:


    fdb_handle* Database::beginTransaction(Transaction* t, sequence &startSequence) {
        std::unique_lock<std::mutex> lock(_file->_transactionMutex);
        while (_file->_transaction != NULL)
            _file->_transactionCond.wait(lock);

        fdb_handle* realHandle = _handle;
        fdb_begin_transaction(realHandle, FDB_ISOLATION_READ_COMMITTED);
        _file->_transaction = t;
        return realHandle;
    }

    void Database::endTransaction(fdb_handle* handle) {
        std::unique_lock<std::mutex> lock(_file->_transactionMutex);
        _handle = handle;
        _file->_transaction = NULL;
        _file->_transactionCond.notify_one();
    }


    Transaction::Transaction(Database* db)
    :_db(*db), _state(0)
    {
        _handle = _db.beginTransaction(this, _startSequence);
    }

    Transaction::~Transaction() {
        fdb_status status = FDB_RESULT_SUCCESS;
        if (_handle) {
            if (_state >= 0) {
                status = fdb_end_transaction(_handle, FDB_COMMIT_NORMAL);
                if (status != FDB_RESULT_SUCCESS)
                    _state = -1;
            } else {
                fdb_abort_transaction(_handle);
            }
        }
        _db.endTransaction(_handle); // return handle back to Database object
        forestdb::check(status); // throw exception if end_transaction failed
    }

    void Transaction::check(fdb_status status) {
        if (status == FDB_RESULT_SUCCESS) {
            if (_state == 0)
                _state = 1;
        } else {
            _state = -1;
            forestdb::check(status); // throw exception
        }
    }

    void Transaction::deleteDatabase() {
        std::string path = _db.filename();
        check(::fdb_close(_handle));
        _handle = NULL;
        if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
            _state = -1;
            check(::fdb_open(&_handle, path.c_str(), &_db._config));
            _db._handle = _handle;
            throw(errno);
        }
        _db.deleted();
    }

    void Transaction::erase() {
        std::string path = _db.filename();
        deleteDatabase();
        check(::fdb_open(&_handle, path.c_str(), &_db._config));
        _db._handle = _handle;
        check(fdb_begin_transaction(_handle, FDB_ISOLATION_READ_COMMITTED)); // re-open it
    }

    void Transaction::rollbackTo(sequence seq) {
        check(fdb_rollback(&_handle, seq));
        _db._handle = _handle;
    }

    void Transaction::compact() {
        std::string path = _db.filename();
        std::string tempPath = path + ".compact";

        check(fdb_end_transaction(_handle, FDB_COMMIT_NORMAL));

        fdb_status status = fdb_compact(_handle, tempPath.c_str());
        if (status != FDB_RESULT_SUCCESS) {
            ::unlink(tempPath.c_str());
            check(status);
        }
        check(::fdb_close(_handle));
        if (::rename(tempPath.c_str(), path.c_str()) < 0) {
            ::unlink(tempPath.c_str());
            check(FDB_RESULT_FILE_RENAME_FAIL);
        }
        check(::fdb_open(&_handle, path.c_str(), &_db._config));
        _db._handle = _handle;
        check(fdb_begin_transaction(_handle, FDB_ISOLATION_READ_COMMITTED)); // re-open it
    }

    void Transaction::commit() {
        check(fdb_commit(_handle, FDB_COMMIT_NORMAL));
    }

    void Transaction::write(Document &doc) {
        check(fdb_set(_handle, doc));
    }

    sequence Transaction::set(slice key, slice meta, slice body) {
        if ((size_t)key.buf & 0x03) {
            // Workaround for unaligned-access crashes on ARM (down in forestdb's crc_32_8 fn)
            void* keybuf = alloca(key.size);
            memcpy(keybuf, key.buf, key.size);
            key.buf = keybuf;
        }
        fdb_doc doc = {
            .key = (void*)key.buf,
            .keylen = key.size,
            .meta = (void*)meta.buf,
            .metalen = meta.size,
            .body  = (void*)body.buf,
            .bodylen = body.size,
        };
        check(fdb_set(_handle, &doc));
        if (meta.buf) {
            Log("DB %p: added %s --> %s (meta %s) (seq %llu)\n",
                    _handle,
                    key.hexString().c_str(),
                    body.hexString().c_str(),
                    meta.hexString().c_str(),
                    doc.seqnum);
        } else {
            Log("DB %p: added %s --> %s (seq %llu)\n",
                    _handle,
                    key.hexString().c_str(),
                    body.hexString().c_str(),
                    doc.seqnum);
        }
        return doc.seqnum;
    }

    bool Transaction::del(forestdb::Document &doc) {
        return checkGet(fdb_del(_handle, doc));
    }

    bool Transaction::del(forestdb::slice key) {
        if ((size_t)key.buf & 0x03) {
            // Workaround for unaligned-access crashes on ARM (down in forestdb's crc_32_8 fn)
            void* keybuf = alloca(key.size);
            memcpy(keybuf, key.buf, key.size);
            key.buf = keybuf;
        }
        fdb_doc doc = {
            .key = (void*)key.buf,
            .keylen = key.size,
        };
        return checkGet(fdb_del(_handle, &doc));
    }

    bool Transaction::del(sequence seq) {
        Document doc;
        doc._doc.seqnum = seq;
        return checkGet(fdb_get_metaonly_byseq(_handle, doc))
            && del(doc);
    }


}