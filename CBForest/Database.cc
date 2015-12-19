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
#include "atomic.h"           // forestdb internal
#include <errno.h>
#include <stdarg.h>           // va_start, va_end
#include <stdio.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>
#ifdef _MSC_VER
#include "asprintf.h"
#endif


namespace cbforest {

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

    void error::_throw(fdb_status status) {
        WarnError("%s (%d)\n", fdb_error_msg(status), status);
        throw error{status};
    }


    void error::assertionFailed(const char *fn, const char *file, unsigned line, const char *expr) {
        if (LogLevel > kError || LogCallback == NULL)
            fprintf(stderr, "Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        WarnError("Assertion failed: %s (%s:%u, in %s)", expr, file, line, fn);
        throw error(error::AssertionFailed);
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


    static void logCallback(int err_code, const char *err_msg, void *ctx_data) {
        // don't warn about read errors: VersionedDocument can trigger them when it looks for a
        // revision that's been compacted away.
        if (err_code == FDB_RESULT_READ_FAIL)
            return;
        WarnError("ForestDB error %d: %s (handle=%p)", err_code, err_msg, ctx_data);
    }

    static Database::config sDefaultConfig;
    static bool sDefaultConfigInitialized = false;

    Database::config Database::defaultConfig() {
        if (!sDefaultConfigInitialized) {
            *(fdb_config*)&sDefaultConfig = fdb_get_default_config();
            sDefaultConfig.purging_interval = 1; // WORKAROUND for ForestDB bug MB-16384
            sDefaultConfig.compaction_cb_mask = FDB_CS_BEGIN | FDB_CS_COMPLETE;
            sDefaultConfigInitialized = true;
        }
        return sDefaultConfig;
    }

    void Database::setDefaultConfig(const Database::config &cfg) {
        check(fdb_init((fdb_config*)&cfg));
        sDefaultConfig = cfg;
    }

    Database::Database(std::string path, const config& cfg)
    :KeyStore(NULL),
     _file(File::forPath(path)),
     _config(cfg),
     _fileHandle(NULL),
     _isCompacting(false)
    {
        _config.compaction_cb = compactionCallback;
        _config.compaction_cb_ctx = this;
        reopen(path);
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

    bool Database::contains(KeyStore& store) const {
        auto i = _kvHandles.find(store.name());
        return i != _kvHandles.end() && i->second == store.handle();
    }


#pragma mark - MUTATING OPERATIONS:


    void Database::reopen(std::string path) {
        check(::fdb_open(&_fileHandle, path.c_str(), &_config));
        check(::fdb_kvs_open_default(_fileHandle, &_handle, NULL));
        fdb_set_log_callback(_handle, logCallback, _handle);
    }

    void Database::deleteDatabase(bool andReopen) {
        Transaction t(this, false);
        std::string path = filename();
        check(::fdb_close(_fileHandle));
        deleted();

        deleteDatabase(path, _config);
        if (andReopen)
            reopen(path);
    }

    void Database::deleteDatabase(std::string path, const config &cfg) {
        check(fdb_destroy(path.c_str(), (config*)&cfg));
    }

    void Database::commit() {
        check(fdb_commit(_fileHandle, FDB_COMMIT_NORMAL));
    }

    void Database::rekey(const fdb_encryption_key &encryptionKey) {
        check(fdb_rekey(_fileHandle, encryptionKey));
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
        CBFAssert(_file->_transaction == t);
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
            cbforest::check(status); // throw exception
        }
    }

#pragma mark - COMPACTION:

    static atomic_uint32_t sCompactCount;

    void (*Database::onCompactCallback)(Database* db, bool compacting);


    void Database::compact() {
        check(fdb_compact(_fileHandle, NULL));
    }

    fdb_compact_decision Database::compactionCallback(fdb_file_handle *fhandle,
                                                      fdb_compaction_status status,
                                                      const char *kv_store_name,
                                                      fdb_doc *doc,
                                                      uint64_t last_oldfile_offset,
                                                      uint64_t last_newfile_offset,
                                                      void *ctx)
    {
        if (((Database*)ctx)->onCompact(status, kv_store_name, doc,
                                        last_oldfile_offset, last_newfile_offset))
            return FDB_CS_KEEP_DOC;
        else
            return FDB_CS_DROP_DOC;
    }

    bool Database::onCompact(fdb_compaction_status status,
                             const char *kv_store_name,
                             fdb_doc *doc,
                             uint64_t last_oldfile_offset,
                             uint64_t last_newfile_offset)
    {
        switch (status) {
            case FDB_CS_BEGIN:
                _isCompacting = true;
                atomic_incr_uint32_t(&sCompactCount);
                Log("Database %p COMPACTING...", this);
                break;
            case FDB_CS_COMPLETE:
                _isCompacting = false;
                atomic_decr_uint32_t(&sCompactCount);
                Log("Database %p END COMPACTING", this);
                break;
            default:
                return true; // skip the onCompactCallback
        }
        if (onCompactCallback)
            onCompactCallback(this, _isCompacting);
        return true;
    }

    bool Database::isAnyCompacting() {
        return atomic_get_uint32_t(&sCompactCount) > 0;
    }

    void Database::setCompactionMode(fdb_compaction_mode_t mode) {
        check(fdb_switch_compaction_mode(_fileHandle, mode,  _config.compaction_threshold));
        _config.compaction_mode = mode;
    }

}
