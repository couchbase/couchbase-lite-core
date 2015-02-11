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
#include "Document.hh"
#include "LogInternal.hh"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>


namespace forestdb {

    static void defaultLogCallback(logLevel level, const char *message) {
        static const char* kLevelNames[4] = {"debug", "info", "WARNING", "ERROR"};
        fprintf(stderr, "CBForest %s: %s\n", kLevelNames[level], message);
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

    static void logCallback(int err_code, const char *err_msg, void *ctx_data) {
        WarnError("ForestDB error %d: %s (handle=%p)", err_code, err_msg, ctx_data);
    }

    Database::Database(std::string path, const config& cfg)
    :KeyStore(NULL),
     _file(File::forPath(path)),
     _config(cfg),
     _fileHandle(NULL)
    {
        check(::fdb_open(&_fileHandle, path.c_str(), (config*)&cfg));
        check(::fdb_kvs_open_default(_fileHandle, &_handle, NULL));
        fdb_set_log_callback(_handle, logCallback, _handle);
    }

    Database::~Database() {
        // fdb_close will automatically close _handle as well.
        if (_fileHandle)
            fdb_close(_fileHandle);
    }

    Database::info Database::getInfo() const {
        info i;
        check(fdb_get_file_info(_fileHandle, &i));
        return i;
    }

    std::string Database::filename() const {
        return std::string(this->getInfo().filename);
    }

    bool Database::isReadOnly() const {
        return (_config.flags & FDB_OPEN_FLAG_RDONLY) != 0;
    }

    void Database::deleted() {
        _fileHandle = NULL;
        _handle = NULL;
    }

    fdb_kvs_handle* Database::openKVS(std::string name) const {
        auto i = _kvHandles.find(name);
        if (i != _kvHandles.end()) {
            return i->second;
        } else {
            fdb_kvs_handle* handle;
            check(fdb_kvs_open(_fileHandle, &handle, name.c_str(),  NULL));
            const_cast<Database*>(this)->_kvHandles[name] = handle;
            return handle;
        }
    }

    void Database::closeKeyStore(std::string name) {
        fdb_kvs_handle* handle = _kvHandles[name];
        if (!handle)
            return;
        check(fdb_kvs_close(handle));
        _kvHandles.erase(name);
    }

    void Database::deleteKeyStore(std::string name) {
        closeKeyStore(name);
        check(fdb_kvs_remove(_fileHandle, name.c_str()));
    }


#pragma mark - MUTATING OPERATIONS:


    void Database::reopen(std::string path) {
        check(::fdb_open(&_fileHandle, path.c_str(), &_config));
    }

    void Database::deleteDatabase(bool andReopen) {
        Transaction t(this, false);
        std::string path = filename();
        check(::fdb_close(_fileHandle));
        deleted();
        check(fdb_destroy(path.c_str(), &_config));
        if (andReopen)
            reopen(path);
    }

    void Database::compact() {
        check(fdb_compact(_fileHandle, NULL));
    }

    void Database::commit() {
        check(fdb_commit(_fileHandle, FDB_COMMIT_NORMAL));
    }


#pragma mark - TRANSACTION:

    void Database::beginTransaction(Transaction* t) {
        std::unique_lock<std::mutex> lock(_file->_transactionMutex);
        while (_file->_transaction != NULL)
            _file->_transactionCond.wait(lock);

        if (t->state() == Transaction::kCommit)
            check(fdb_begin_transaction(_fileHandle, FDB_ISOLATION_READ_COMMITTED));
        _file->_transaction = t;
    }

    void Database::endTransaction(Transaction* t) {
        fdb_status status = FDB_RESULT_SUCCESS;
        switch (t->state()) {
            case Transaction::kCommit:
                status = fdb_end_transaction(_fileHandle, FDB_COMMIT_NORMAL);
                break;
            case Transaction::kAbort:
                (void)fdb_abort_transaction(_fileHandle);
                break;
            case Transaction::kNoOp:
                break;
        }

        std::unique_lock<std::mutex> lock(_file->_transactionMutex);
        assert(_file->_transaction == t);
        _file->_transaction = NULL;
        _file->_transactionCond.notify_one();

        check(status);
    }


    Transaction::Transaction(Database* db)
    :KeyStoreWriter(*db),
     _db(*db),
     _state(kCommit)
    {
        _db.beginTransaction(this);
    }

    Transaction::Transaction(Database* db, bool begin)
    :KeyStoreWriter(*db),
     _db(*db),
     _state(begin ? kCommit : kNoOp)
    {
        _db.beginTransaction(this);
    }

    Transaction::~Transaction() {
        _db.endTransaction(this);
    }

    void Transaction::check(fdb_status status) {
        if (status != FDB_RESULT_SUCCESS) {
            _state = kAbort;
            forestdb::check(status); // throw exception
        }
    }

}