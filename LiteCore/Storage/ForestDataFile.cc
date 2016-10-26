//
//  ForestDataFile.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/25/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "ForestDataFile.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "forestdb.h"
#include <algorithm>
#include <thread>


// This constant is used by fdb_get_byoffset but not exposed in fdb_types.h.
#define SEQNUM_NOT_USED (fdb_seqnum_t)(-1ll)

using namespace std;


namespace litecore {

    // Size of ForestDB buffer cache allocated for a database
    static const size_t kDBBufferCacheSize = (8*1024*1024);

    // ForestDB Write-Ahead Log size (# of records)
    static const size_t kDBWALThreshold = 1024;

    // How often ForestDB should check whether databases need auto-compaction
    static const uint64_t kAutoCompactInterval = (5*60);

    static inline void check(fdb_status status) {
        if (_usuallyFalse(status != FDB_RESULT_SUCCESS))
            error::_throw(error::ForestDB, status);
    }

    static bool checkGet(fdb_status status) {
        if (status == FDB_RESULT_KEY_NOT_FOUND)
            return false;
        check(status);
        return true;
    }


#pragma mark - CONFIG:


    static fdb_config sDefaultConfig;
    static atomic<bool> sDefaultConfigInitialized;

    fdb_config ForestDataFile::defaultConfig() {
        if (!sDefaultConfigInitialized) {
            *(fdb_config*)&sDefaultConfig = fdb_get_default_config();

            // Global configuration:
            sDefaultConfig.buffercache_size = kDBBufferCacheSize;
//            sDefaultConfig.compress_document_body = true; // Disabled: it hurts performance
            sDefaultConfig.compactor_sleep_duration = kAutoCompactInterval;
            sDefaultConfig.num_compactor_threads = 1;
            sDefaultConfig.num_bgflusher_threads = 1;
            // Per-database configuration:
            sDefaultConfig.wal_threshold = kDBWALThreshold;
            sDefaultConfig.wal_flush_before_commit = true;
            sDefaultConfig.seqtree_opt = FDB_SEQTREE_USE;
            sDefaultConfig.purging_interval = 1;
            sDefaultConfig.compaction_mode = FDB_COMPACTION_AUTO;
            sDefaultConfig.compaction_cb_mask = FDB_CS_BEGIN | FDB_CS_COMPLETE;

            sDefaultConfigInitialized = true;
        }
        return sDefaultConfig;
    }


    void ForestDataFile::setDefaultConfig(const fdb_config &cfg) {
        check(fdb_init((fdb_config*)&cfg));
        sDefaultConfig = cfg;
    }


    static fdb_encryption_key forestEncryptionKey(EncryptionAlgorithm alg,
                                                  slice key) {
        fdb_encryption_key fdbKey;
        switch (alg) {
            case kNoEncryption:
                fdbKey.algorithm = FDB_ENCRYPTION_NONE;
                break;
            case kAES256:
                if(key.buf == nullptr || key.size != sizeof(fdbKey.bytes))
                    error::_throw(error::InvalidParameter);
                fdbKey.algorithm = FDB_ENCRYPTION_AES256;
                memcpy(fdbKey.bytes, key.buf, sizeof(fdbKey.bytes));
                break;
            default:
                error::_throw(error::UnsupportedEncryption);
        }
        return fdbKey;
    }


    static void setConfigOptions(fdb_config &config, const DataFile::Options *options) {
        if (!options)
            return;
        if (options->writeable)
            config.flags &= ~FDB_OPEN_FLAG_RDONLY;
        else
            config.flags |= FDB_OPEN_FLAG_RDONLY;
        if (options->create)
            config.flags |= FDB_OPEN_FLAG_CREATE;
        else
            config.flags &= ~FDB_OPEN_FLAG_CREATE;
        config.seqtree_opt = options->keyStores.sequences ? FDB_SEQTREE_USE
        : FDB_SEQTREE_NOT_USE;

        // If purging_interval is 0, deleted ForestDB docs vanish pretty much instantly (_not_
        // "at the next replication" as the ForestDB header says.) A value of > 0 makes them
        // stick around until the next compaction.
        if (options->keyStores.softDeletes)
            config.purging_interval = max(config.purging_interval, 1u);
        else
            config.purging_interval = 0;

        config.encryption_key = forestEncryptionKey(options->encryptionAlgorithm,
                                                    options->encryptionKey);
    }


#pragma mark - FACTORY:


    bool ForestDataFile::Factory::encryptionEnabled(EncryptionAlgorithm alg) {
        return (alg == kNoEncryption || alg == kAES256);
    }

    ForestDataFile* ForestDataFile::Factory::openFile(const FilePath &path, const Options *options) {
        return new ForestDataFile(path, options);
    }

    bool ForestDataFile::Factory::deleteFile(const FilePath &path, const DataFile::Options* options) {
        fdb_config cfg = ForestDataFile::defaultConfig();
        setConfigOptions(cfg, options);
        cfg.compaction_cb = ForestDataFile::compactionCallback;
        cfg.compaction_cb_ctx = nullptr;
        fdb_status status = FDB_RESULT_SUCCESS;
        for (int retry = 100; retry > 0; --retry) {
            status = fdb_destroy(path.path().c_str(), &cfg);
            if (status == FDB_RESULT_IN_USE_BY_COMPACTOR)
                std::this_thread::sleep_for(std::chrono::microseconds(100 * 1000));
            else
                break;
        }
        if (status == FDB_RESULT_NO_SUCH_FILE)
            return false;
        check(status);
        (void)path.del();     // Delete the path with no extensions too, to make sure (see #9)
        return true;
    }

    bool ForestDataFile::Factory::fileExists(const FilePath &path) {
        return path.exists() || path.addingExtension(".meta").exists();
    }

    ForestDataFile::Factory& ForestDataFile::factory() {
        static ForestDataFile::Factory s;
        return s;
    }


#pragma mark - DATA FILE:


    ForestDataFile::ForestDataFile(const FilePath &path,
                                   const DataFile::Options *options)
    :ForestDataFile(path, options, defaultConfig())
    { }


    ForestDataFile::ForestDataFile(const FilePath &path,
                                   const DataFile::Options *options,
                                   const fdb_config& cfg)
    :DataFile(path, options),
     _config(cfg)
    {
        setConfigOptions(_config, options);
        _config.compaction_cb = compactionCallback;
        _config.compaction_cb_ctx = this;
        reopen();
    }

    ForestDataFile::~ForestDataFile() {
        if (_fileHandle) {
            try {
                close();
            } catch (...) {
                Warn("ForestDataFile: Unexpected error while closing");
            }
        }
    }

    fdb_file_info ForestDataFile::info() const {
        fdb_file_info i;
        check(fdb_get_file_info(_fileHandle, &i));
        return i;
    }

    void ForestDataFile::shutdown() {
        check(fdb_shutdown());
    }


#pragma mark - OPEN/CLOSE/DELETE:


    bool ForestDataFile::isOpen() const noexcept {
        return _fileHandle != nullptr;
    }


    void ForestDataFile::close() {
        DataFile::close(); // closes all the KeyStores
        if (_fileHandle) {
            check(::fdb_close(_fileHandle));
            _fileHandle = nullptr;
        }
    }

    void ForestDataFile::reopen() {
        Assert(!isOpen());
        string path = filePath().path();
        const char *cpath = path.c_str();
        LogToAt(DBLog, Debug, "ForestDataFile: open %s", cpath);
        auto status = ::fdb_open(&_fileHandle, cpath, &_config);
        if (status == FDB_RESULT_INVALID_COMPACTION_MODE
                && _config.compaction_mode == FDB_COMPACTION_AUTO) {
            // DataFile didn't use to have autocompact; open it the old way and update it:
            auto config = _config;
            config.compaction_mode = FDB_COMPACTION_MANUAL;
            check(fdb_open(&_fileHandle, cpath, &config));
            setAutoCompact(true);
        } else {
            check(status);
        }
    }

    void ForestDataFile::deleteDataFile() {
        close();
        factory().deleteFile(filePath(), &options());
    }

    void ForestDataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
        fdb_encryption_key fdbKey = forestEncryptionKey(alg, newKey);
        check(fdb_rekey(_fileHandle, fdbKey));
        _config.encryption_key = fdbKey;
    }


    void ForestDataFile::_beginTransaction(Transaction*) {
        check(fdb_begin_transaction(_fileHandle, FDB_ISOLATION_READ_COMMITTED));
    }


    void ForestDataFile::_endTransaction(Transaction *t, bool commit) {
        if (commit) {
            LogTo(DBLog, "ForestDataFile: commit transaction");
            check(fdb_end_transaction(_fileHandle, FDB_COMMIT_NORMAL));
        } else {
            LogTo(DBLog, "ForestDataFile: abort transaction");
            (void)fdb_abort_transaction(_fileHandle);
        }
    }


#pragma mark - COMPACTION:


    void ForestDataFile::compact() {
        auto status = fdb_compact(_fileHandle, nullptr);
        if (status == FDB_RESULT_FILE_IS_BUSY) {
            // This result means there is already a background auto-compact in progress.
            while (isCompacting())
                std::this_thread::sleep_for(std::chrono::microseconds(100 * 1000));
        } else {
            check(status);
        }
    }

    // static
    fdb_compact_decision ForestDataFile::compactionCallback(fdb_file_handle *fhandle,
                                                      fdb_compaction_status status,
                                                      const char *kv_store_name,
                                                      fdb_doc *doc,
                                                      uint64_t last_oldfile_offset,
                                                      uint64_t last_newfile_offset,
                                                      void *ctx)
    {
        if (((ForestDataFile*)ctx)->onCompact(status, kv_store_name, doc,
                                              last_oldfile_offset, last_newfile_offset))
            return FDB_CS_KEEP_DOC;
        else
            return FDB_CS_DROP_DOC;
    }

    bool ForestDataFile::onCompact(fdb_compaction_status status,
                             const char *kv_store_name,
                             fdb_doc *doc,
                             uint64_t last_oldfile_offset,
                             uint64_t last_newfile_offset)
    {
        switch (status) {
            case FDB_CS_BEGIN:
                LogTo(DBLog, "ForestDataFile %p COMPACTING...", this);
                beganCompacting();
                break;
            case FDB_CS_COMPLETE:
                {
                    Transaction t(this);
                    updatePurgeCount(t);
                    t.commit();
                }
                LogTo(DBLog, "ForestDataFile %p END COMPACTING", this);
                finishedCompacting();
                break;
            default:
                break;
        }
        return true;
    }

    bool ForestDataFile::setAutoCompact(bool autoCompact) {
        auto mode = (autoCompact ? FDB_COMPACTION_AUTO : FDB_COMPACTION_MANUAL);
        check(fdb_switch_compaction_mode(_fileHandle, mode, _config.compaction_threshold));
        _config.compaction_mode = mode;
        return true;
    }


#pragma mark - KEY-STORES:


    KeyStore* ForestDataFile::newKeyStore(const string &name, KeyStore::Capabilities options) {
        return new ForestKeyStore(*this, name, options);
    }

    void ForestDataFile::deleteKeyStore(const string &name) {
        check(fdb_kvs_remove(_fileHandle, name.c_str()));
    }


    vector<string> ForestDataFile::allKeyStoreNames() {
        fdb_kvs_name_list list;
        check(fdb_get_kvs_name_list(_fileHandle, &list));
        vector<string> names;
        names.reserve(list.num_kvs_names);
        for (size_t i = 0; i < list.num_kvs_names; ++i)
            names.push_back(string(list.kvs_names[i]));
        fdb_free_kvs_name_list(&list);
        return names;
    }

    
    static void logCallback(int err_code, const char *err_msg, void *ctx_data) {
        WarnError("ForestDB error %d: %s (fdb_kvs_handle=%p)", err_code, err_msg, ctx_data);
    }


    ForestKeyStore::ForestKeyStore(ForestDataFile &db, const string &name, KeyStore::Capabilities capabilities)
    :KeyStore(db, name, capabilities)
    {
        capabilities.getByOffset = true;
        reopen();
    }

    ForestKeyStore::~ForestKeyStore() {
        if (_handle)
            fdb_kvs_close(_handle);
    }

    void ForestKeyStore::reopen() {
        if (!_handle) {
            auto &db = (ForestDataFile&)dataFile();
            check(fdb_kvs_open(db._fileHandle, &_handle, name().c_str(),  nullptr));
            (void)fdb_set_log_callback(_handle, logCallback, _handle);
        }
    }


    void ForestKeyStore::close() {
        if (_handle) {
            check(fdb_kvs_close(_handle));
            _handle = nullptr;
        }
    }


    uint64_t ForestKeyStore::recordCount() const {
        fdb_kvs_info info;
        check(fdb_get_kvs_info(_handle, &info));
        return info.doc_count;
    }


    sequence ForestKeyStore::lastSequence() const {
        fdb_seqnum_t seq;
        check(fdb_get_kvs_seqnum(_handle, &seq));
        return seq;
    }


    void ForestKeyStore::setDocNoKey(Record &rec, fdb_doc &fdoc) const {
        rec.adoptMeta(slice(fdoc.meta, fdoc.metalen));
        if (fdoc.body)
            rec.adoptBody(slice(fdoc.body, fdoc.bodylen));
        else
            rec.setUnloadedBodySize(fdoc.bodylen);
        rec.setDeleted(fdoc.deleted);
        updateDoc(rec, fdoc.seqnum, fdoc.offset, fdoc.deleted);
    }

    void ForestKeyStore::setDoc(Record &rec, fdb_doc &fdoc) const {
        rec.adoptKey(slice(fdoc.key, fdoc.keylen));
        setDocNoKey(rec, fdoc);
    }


    bool ForestKeyStore::read(Record &rec, ContentOptions options) const {
        fdb_doc fdoc = {};
        fdoc.key = (void*)rec.key().buf;
        fdoc.keylen = rec.key().size;
        fdb_status status;
        if (options & kMetaOnly)
            status = fdb_get_metaonly(_handle, &fdoc);
        else
            status = fdb_get(_handle, &fdoc);
        if (!checkGet(status))
            return false;
        setDocNoKey(rec, fdoc);
        // (the heap blocks pointed to by fdoc have been adopted by rec, so don't free them.)
        return true;
    }


    void ForestKeyStore::readBody(Record &rec) const {
        if (rec.body().buf)
            return;
        
        if (rec.offset() > 0) {
            slice existingKey = rec.key();
            fdb_doc fdoc = {};
            fdoc.offset = rec.offset();
            fdoc.key = (void*)existingKey.buf;
            fdoc.keylen = existingKey.size;
            fdoc.seqnum = rec.sequence();
            if (!fdoc.seqnum) {
                fdoc.seqnum = SEQNUM_NOT_USED;
            }
            
            check(fdb_get_byoffset(_handle, &fdoc));

            rec.adoptBody(slice(fdoc.body, fdoc.bodylen));
            if (fdoc.key != existingKey.buf)
                free(fdoc.key);
            free(fdoc.meta);
        } else {
            KeyStore::readBody(rec);
        }
    }


    Record ForestKeyStore::get(sequence seq, ContentOptions options) const {
        Record rec;
        fdb_doc fdoc = {};
        fdoc.seqnum = seq;
        fdb_status status;
        if (options & kMetaOnly)
            status = fdb_get_metaonly_byseq(_handle, &fdoc);
        else
            status = fdb_get_byseq(_handle, &fdoc);
        if (checkGet(status)) {
            setDoc(rec, fdoc);
        }
        // (the heap blocks pointed to by fdoc have been adopted by rec, so don't free them.)
        return rec;
    }


    Record ForestKeyStore::getByOffsetNoErrors(uint64_t offset, sequence seq) const {
        Record result;

        fdb_doc fdoc = {};
        fdoc.offset = offset;
        fdoc.seqnum = seq;
        if (fdb_get_byoffset(_handle, &fdoc) == FDB_RESULT_SUCCESS)
            setDoc(result, fdoc);
        return result;
    }



    KeyStore::setResult ForestKeyStore::set(slice key, slice meta, slice body, Transaction&) {
        LogTo(DBLog, "KeyStore(%s) set %s", name().c_str(), logSlice(key));
        fdb_doc fdoc = {
            key.size, meta.size, body.size, 0,
            (void*)key.buf,
            0, 0,
            (void*)meta.buf, (void*)body.buf,
            false,
            0
        };
        check(fdb_set(_handle, &fdoc));
        return {fdoc.seqnum, fdoc.offset};
    }


    bool ForestKeyStore::_del(slice key, Transaction&) {
        fdb_doc fdoc = { };
        fdoc.key = (void*)key.buf;
        fdoc.keylen = key.size;
        return checkGet(fdb_del(_handle, &fdoc));
    }


    bool ForestKeyStore::_del(sequence s, Transaction&) {
        fdb_doc fdoc = { };
        fdoc.seqnum = s;
        return checkGet(fdb_get_metaonly_byseq(_handle, &fdoc))
            && checkGet(fdb_del(_handle, &fdoc));
    }


    void ForestKeyStore::erase() {
        check(fdb_rollback(&_handle, 0));
    }


#pragma mark - ITERATORS:


    class ForestEnumerator : public RecordEnumerator::Impl {
    public:
        ForestEnumerator(ForestKeyStore &store, fdb_iterator *iterator, RecordEnumerator::Options &options)
        :_store(store),
         _iterator(iterator),
         _descending(options.descending),
         _metaOnly((options.contentOptions & kMetaOnly) != 0)
        {
            if (_descending)
                fdb_iterator_seek_to_max(iterator);  // ignore err; fails if max key doesn't exist
        }


        ~ForestEnumerator() {
            fdb_iterator_close(_iterator);
        }


        virtual bool shouldSkipFirstStep() override {
            return true;    // iterator is already positioned at first record when created
        }


        bool next() override {
            auto fn = _descending ? fdb_iterator_prev : fdb_iterator_next;
            auto status = fn(_iterator);
            if (status == FDB_RESULT_ITERATOR_FAIL)
                return false;
            check(status);
            return true;
        }

        bool read(Record &rec) override {
            auto fn = (_metaOnly ? fdb_iterator_get_metaonly : fdb_iterator_get);
            fdb_doc fdoc = { };
            fdb_doc *docP = &fdoc;
            fdb_status status = fn(_iterator, &docP);
            if (status == FDB_RESULT_ITERATOR_FAIL)
                return false;
            check(status);
            _store.setDoc(rec, fdoc);
            LogToAt(DBLog, Debug, "enum:     fdb_iterator_get --> [%s]", rec.key().hexCString());
            return true;
        }

    private:
        ForestKeyStore &_store;
        fdb_iterator *_iterator {nullptr};
        bool _descending;
        bool _metaOnly;
    };



    static fdb_iterator_opt_t iteratorOptions(const RecordEnumerator::Options& options) {
        fdb_iterator_opt_t fdbOptions = 0;
        if (!options.includeDeleted)
            fdbOptions |= FDB_ITR_NO_DELETES;
        if (!options.inclusiveEnd)
            fdbOptions |= (options.descending ? FDB_ITR_SKIP_MIN_KEY : FDB_ITR_SKIP_MAX_KEY);
        if (!options.inclusiveStart)
            fdbOptions |= (options.descending ? FDB_ITR_SKIP_MAX_KEY : FDB_ITR_SKIP_MIN_KEY);
        return fdbOptions;
    }


    RecordEnumerator::Impl* ForestKeyStore::newEnumeratorImpl(slice minKey, slice maxKey,
                                                     RecordEnumerator::Options &options)
    {
        fdb_iterator *iterator;
        check(fdb_iterator_init(_handle, &iterator,
                                minKey.buf, minKey.size,
                                maxKey.buf, maxKey.size,
                                iteratorOptions(options)));
        return new ForestEnumerator(*this, iterator, options);
    }


    RecordEnumerator::Impl* ForestKeyStore::newEnumeratorImpl(sequence minSeq, sequence maxSeq,
                                                     RecordEnumerator::Options &options)
    {
        fdb_iterator *iterator;
        check(fdb_iterator_sequence_init(_handle, &iterator,
                                         minSeq, maxSeq,
                                         iteratorOptions(options)));
        return new ForestEnumerator(*this, iterator, options);
    }

}
