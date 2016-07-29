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
#include "time_utils.h"       // forestdb internal
#include <errno.h>
#include <stdarg.h>           // va_start, va_end
#include <stdio.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>
#ifdef _MSC_VER
#include "asprintf.h"
#elif __ANDROID__
#include <android/log.h>
#endif


namespace cbforest {

    static void defaultLogCallback(logLevel level, const char *message) {
#ifdef __ANDROID__
        static const int kLevels[4] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        __android_log_write(kLevels[level], "CBForest", message);
#else
        static const char *kLevelNames[4] = {"debug", "info", "WARNING", "ERROR"};
        fprintf(stderr, "CBForest %s: %s\n", kLevelNames[level], message);
#endif
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
        File(std::string path)      :_path(path) { }
        const std::string _path;
        std::mutex _transactionMutex;
        std::condition_variable _transactionCond;
        Transaction* _transaction {NULL};

        static std::unordered_map<std::string, File*> sFileMap;
        static std::mutex sMutex;
    };

    std::unordered_map<std::string, Database::File*> Database::File::sFileMap;
    std::mutex Database::File::sMutex;

    Database::File* Database::File::forPath(std::string path) {
        std::unique_lock<std::mutex> lock(sMutex);
        File* file = sFileMap[path];
        if (!file) {
            file = new File(path);
            sFileMap[path] = file;
        }
        return file;
    }


#pragma mark - DATABASE:


    static Database::config sDefaultConfig;
    static bool sDefaultConfigInitialized = false;

    Database::config Database::defaultConfig() {
        if (!sDefaultConfigInitialized) {
            *(fdb_config*)&sDefaultConfig = fdb_get_default_config();
            // Why a nonzero purging_interval? We want deleted ForestDB docs to stick around
            // for a little while so the MapReduceIndexer can see them next time it updates its
            // index, and clean out rows emitted by those docs. If purging_interval is 0,
            // deleted docs vanish pretty much instantly (_not_ "at the next replication" as
            // the ForestDB header says.) A value of >0 makes them stick around until the next
            // compaction.
            sDefaultConfig.purging_interval = 1;
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
     _config(cfg)
    {
        _config.compaction_cb = compactionCallback;
        _config.compaction_cb_ctx = this;
        reopen();
    }

    Database::~Database() {
        Debug("Database: deleting (~Database)");
        CBFAssert(!_inTransaction);
        if (_fileHandle) {
            ::fdb_close(_fileHandle);
            // FYI: fdb_close will automatically close _handle as well.
        }
    }

    Database::info Database::getInfo() const {
        info i;
        check(fdb_get_file_info(_fileHandle, &i));
        return i;
    }

    const std::string& Database::filename() const {
        return _file->_path;
    }

    bool Database::isReadOnly() const {
        return (_config.flags & FDB_OPEN_FLAG_RDONLY) != 0;
    }

#pragma mark KEY-STORES:

    KeyStore& Database::getKeyStore(std::string name) const {
        if (name.empty())
            return *const_cast<Database*>(this);
        auto i = _keyStores.find(name);
        if (i != _keyStores.end() && i->second) {
            return *i->second;
        } else {
            Debug("Database: open KVS '%s'", name.c_str());
            fdb_kvs_handle* handle;
            check(fdb_kvs_open(_fileHandle, &handle, name.c_str(),  NULL));
            if (i != _keyStores.end()) {
                // Reopening
                i->second->_handle = handle;
                return *i->second;
            } else {
                auto store = new KeyStore(handle);
                const_cast<Database*>(this)->_keyStores[name].reset(store);
                store->enableErrorLogs(true);
                return *store;
            }
        }
    }

    void Database::closeKeyStore(std::string name) {
        Debug("Database: close KVS '%s'", name.c_str());
        auto i = _keyStores.find(name);
        if (i != _keyStores.end()) {
            // Never remove a KeyStore from _keyStores: there may be objects pointing to it 
            i->second->close();
        }
    }

    void Database::deleteKeyStore(std::string name) {
        closeKeyStore(name);
        check(fdb_kvs_remove(_fileHandle, name.c_str()));
    }

    bool Database::contains(KeyStore& store) const {
        if (store.handle() == _handle)
            return true;
        auto i = _keyStores.find(store.name());
        return i != _keyStores.end() && i->second->handle() == store.handle();
    }


#pragma mark PURGE/DELETION COUNT:


    static const char* const kInfoStoreName = "info";
    static const char* const kDeletionCountKey = "deletionCount";
    static const char* const kPurgeCountKey = "purgeCount";

    static uint64_t readCount(const Document &doc) {
        uint64_t count;
        if (doc.body().size < sizeof(count))
            return 0;
        memcpy(&count, doc.body().buf, sizeof(count));
        return _endian_decode(count);
    }

    void Database::incrementDeletionCount(Transaction *t) {
        KeyStore &infoStore = getKeyStore(kInfoStoreName);
        Document doc = infoStore.get(slice(kDeletionCountKey));
        uint64_t purgeCount = readCount(doc) + 1;
        uint64_t newBody = _endian_encode(purgeCount);
        doc.setBody(slice(&newBody, sizeof(newBody)));
        KeyStoreWriter(infoStore, *t).write(doc);
    }

    uint64_t Database::purgeCount() const {
        KeyStore &infoStore = getKeyStore(kInfoStoreName);
        return readCount( infoStore.get(slice(kPurgeCountKey)) );
    }

    void Database::updatePurgeCount() {
        KeyStore& infoStore = getKeyStore(kInfoStoreName);
        Document purgeCount = infoStore.get(slice(kDeletionCountKey));
        if (purgeCount.exists()) {
            KeyStoreWriter infoWriter(infoStore);
            infoWriter.set(slice(kPurgeCountKey), purgeCount.body());
        }
    }


#pragma mark - MUTATING OPERATIONS:


    void Database::close() {
        if (_fileHandle)
            check(::fdb_close(_fileHandle));
        _fileHandle = NULL;
        // fdb_close implicitly closes all the kv handles, so null them out:
        _handle = NULL;
        for (auto i = _keyStores.begin(); i != _keyStores.end(); ++i)
            i->second->_handle = NULL;
    }

    void Database::reopen() {
        CBFAssert(!isOpen());
        const char *cpath = _file->_path.c_str();
        Debug("Database: open %s", cpath);
        check(::fdb_open(&_fileHandle, cpath, &_config));
        check(::fdb_kvs_open_default(_fileHandle, &_handle, NULL));
        enableErrorLogs(true);
    }

    void Database::deleteDatabase() {
        if (isOpen()) {
            Transaction t(this, false);
            close();
            deleteDatabase(_file->_path, _config);
        } else {
            deleteDatabase(_file->_path, _config);
        }
    }

    /*static*/ void Database::deleteDatabase(std::string path, const config &cfg) {
        check(fdb_destroy(path.c_str(), (config*)&cfg));
    }

    void Database::rekey(const fdb_encryption_key &encryptionKey) {
        check(fdb_rekey(_fileHandle, encryptionKey));
        _config.encryption_key = encryptionKey;
    }


#pragma mark - TRANSACTION:

    void Database::beginTransaction(Transaction* t) {
        CBFAssert(!_inTransaction);
        if (!isOpen())
            error::_throw(FDB_RESULT_INVALID_HANDLE);
        std::unique_lock<std::mutex> lock(_file->_transactionMutex);
        while (_file->_transaction != NULL)
            _file->_transactionCond.wait(lock);

        if (t->state() >= Transaction::kCommit) {
            Log("Database: beginTransaction");
            check(fdb_begin_transaction(_fileHandle, FDB_ISOLATION_READ_COMMITTED));
        }
        _file->_transaction = t;
        _inTransaction = true;
    }

    void Database::endTransaction(Transaction* t) {
        fdb_status status = FDB_RESULT_SUCCESS;
        switch (t->state()) {
            case Transaction::kCommit:
                Log("Database: commit transaction");
                status = fdb_end_transaction(_fileHandle, FDB_COMMIT_NORMAL);
                break;
            case Transaction::kCommitManualWALFlush:
                Log("Database: commit transaction with WAL flush");
                status = fdb_end_transaction(_fileHandle, FDB_COMMIT_MANUAL_WAL_FLUSH);
                break;
            case Transaction::kAbort:
                Log("Database: abort transaction");
                (void)fdb_abort_transaction(_fileHandle);
                break;
            case Transaction::kNoOp:
                Log("Database: end noop transaction");
                break;
        }

        std::unique_lock<std::mutex> lock(_file->_transactionMutex);
        CBFAssert(_file->_transaction == t);
        _file->_transaction = NULL;
        _file->_transactionCond.notify_one();
        _inTransaction = false;

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

    void Transaction::check(fdb_status status) {
        if (expected(status != FDB_RESULT_SUCCESS, false)) {
            _state = kAbort;
            cbforest::check(status); // throw exception
        }
    }


    bool Transaction::del(slice key) {
        if (!KeyStoreWriter::del(key))
            return false; 
        _db.incrementDeletionCount(this);
        return true;
    }

    bool Transaction::del(Document &doc) {
        return del(doc.key());
    }

#pragma mark - COMPACTION:

    static atomic_uint32_t sCompactCount;

    void Database::compact() {
        auto status = fdb_compact(_fileHandle, NULL);
        if (status == FDB_RESULT_FILE_IS_BUSY) {
            // This result means there is already a background auto-compact in progress.
            while (isCompacting())
                ::usleep(100 * 1000);
        } else {
            check(status);
        }
    }

    // static
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
                updatePurgeCount();
                _isCompacting = false;
                atomic_decr_uint32_t(&sCompactCount);
                Log("Database %p END COMPACTING", this);
                break;
            default:
                return true; // skip the onCompactCallback
        }
        if (_onCompactCallback)
            _onCompactCallback(_onCompactContext, _isCompacting);
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
