//
//  ForestDatabase.cc
//  CBNano
//
//  Created by Jens Alfke on 7/25/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "ForestDatabase.hh"
#include "Document.hh"
#include "DocEnumerator.hh"
#include "Error.hh"
#include "LogInternal.hh"
#include "forestdb.h"
#include <algorithm>
#include <unistd.h>

// This constant is used by fdb_get_byoffset but not exposed in fdb_types.h.
#define SEQNUM_NOT_USED (fdb_seqnum_t)(-1ll)

using namespace std;


namespace cbforest {

    // Size of ForestDB buffer cache allocated for a database
    static const size_t kDBBufferCacheSize = (8*1024*1024);

    // ForestDB Write-Ahead Log size (# of records)
    static const size_t kDBWALThreshold = 1024;

    // How often ForestDB should check whether databases need auto-compaction
    static const uint64_t kAutoCompactInterval = (5*60);

    static inline void check(fdb_status status) {
        if (expected(status != FDB_RESULT_SUCCESS, false))
            error::_throw(error::ForestDB, status);
    }

    static bool checkGet(fdb_status status) {
        if (status == FDB_RESULT_KEY_NOT_FOUND)
            return false;
        check(status);
        return true;
    }


    static fdb_config sDefaultConfig;
    static atomic<bool> sDefaultConfigInitialized;

    fdb_config ForestDatabase::defaultConfig() {
        if (!sDefaultConfigInitialized) {
            *(fdb_config*)&sDefaultConfig = fdb_get_default_config();

            // Global configuration:
            sDefaultConfig.buffercache_size = kDBBufferCacheSize;
            sDefaultConfig.compress_document_body = true;
            sDefaultConfig.compactor_sleep_duration = kAutoCompactInterval;
            sDefaultConfig.num_compactor_threads = 1;
            sDefaultConfig.num_bgflusher_threads = 1;
            // Per-database configuration:
            sDefaultConfig.wal_threshold = kDBWALThreshold;
            sDefaultConfig.wal_flush_before_commit = true;
            sDefaultConfig.seqtree_opt = FDB_SEQTREE_USE;
            sDefaultConfig.purging_interval = 1;
            sDefaultConfig.compaction_cb_mask = FDB_CS_BEGIN | FDB_CS_COMPLETE;

            sDefaultConfigInitialized = true;
        }
        return sDefaultConfig;
    }


    void ForestDatabase::setDefaultConfig(const fdb_config &cfg) {
        check(fdb_init((fdb_config*)&cfg));
        sDefaultConfig = cfg;
    }


    ForestDatabase::ForestDatabase(const string &path, const Database::Options *options)
    :ForestDatabase(path, options, defaultConfig())
    { }


    ForestDatabase::ForestDatabase(const string &path, const Database::Options *options, const fdb_config& cfg)
    :Database(path, options),
     _config(cfg)
    {
        if (options) {
            if (options->writeable)
                _config.flags |= FDB_OPEN_FLAG_CREATE;
            else
                _config.flags &= ~FDB_OPEN_FLAG_CREATE;
            if (options->create)
                _config.flags |= FDB_OPEN_FLAG_CREATE;
            else
                _config.flags &= ~FDB_OPEN_FLAG_CREATE;
            _config.seqtree_opt = options->keyStores.sequences ? FDB_SEQTREE_USE : FDB_SEQTREE_NOT_USE;

            // If purging_interval is 0, deleted ForestDB docs vanish pretty much instantly (_not_
            // "at the next replication" as the ForestDB header says.) A value of > 0 makes them
            // stick around until the next compaction.
            if (options->keyStores.softDeletes)
                _config.purging_interval = max(_config.purging_interval, 1u);
            else
                _config.purging_interval = 0;
        }
        _config.compaction_cb = compactionCallback;
        _config.compaction_cb_ctx = this;
        reopen();
    }

    ForestDatabase::~ForestDatabase() {
        if (_fileHandle) {
            try {
                close();
            } catch (...) {
                Warn("ForestDatabase: Unexpected error while closing");
            }
        }
    }

    fdb_file_info ForestDatabase::info() const {
        fdb_file_info i;
        check(fdb_get_file_info(_fileHandle, &i));
        return i;
    }


#pragma mark - OPEN/CLOSE/DELETE:


    bool ForestDatabase::isOpen() const {
        return _fileHandle != nullptr;
    }


    void ForestDatabase::close() {
        Database::close(); // closes all the KeyStores
        if (_fileHandle) {
            check(::fdb_close(_fileHandle));
            _fileHandle = NULL;
        }
    }

    void ForestDatabase::reopen() {
        CBFAssert(!isOpen());
        const char *cpath = filename().c_str();
        Debug("ForestDatabase: open %s", cpath);
        check(::fdb_open(&_fileHandle, cpath, &_config));
    }

    void ForestDatabase::deleteDatabase() {
        if (isOpen()) {
            //Transaction t(this, false);
            close();
            deleteDatabase(filename(), _config);
        } else {
            deleteDatabase(filename(), _config);
        }
    }

    /*static*/ void ForestDatabase::deleteDatabase(const string &path, const fdb_config &cfg) {
        auto cfg2 = cfg;
        cfg2.compaction_cb = compactionCallback;
        cfg2.compaction_cb_ctx = NULL;
        check(fdb_destroy(path.c_str(), (fdb_config*)&cfg));
    }

    void ForestDatabase::rekey(const fdb_encryption_key &encryptionKey) {
        check(fdb_rekey(_fileHandle, encryptionKey));
        _config.encryption_key = encryptionKey;
    }


    void ForestDatabase::_beginTransaction(Transaction*) {
        check(fdb_begin_transaction(_fileHandle, FDB_ISOLATION_READ_COMMITTED));
    }


    void ForestDatabase::_endTransaction(Transaction *t) {
        fdb_status status = FDB_RESULT_SUCCESS;
        switch (t->state()) {
            case Transaction::kCommit:
                Log("ForestDatabase: commit transaction");
                status = fdb_end_transaction(_fileHandle, FDB_COMMIT_NORMAL);
                break;
            case Transaction::kCommitManualWALFlush:
                Log("ForestDatabase: commit transaction with WAL flush");
                status = fdb_end_transaction(_fileHandle, FDB_COMMIT_MANUAL_WAL_FLUSH);
                break;
            case Transaction::kAbort:
                Log("ForestDatabase: abort transaction");
                (void)fdb_abort_transaction(_fileHandle);
                break;
            case Transaction::kNoOp:
                Log("ForestDatabase: end noop transaction");
                break;
        }
        check(status);
    }


#pragma mark - COMPACTION:


    static atomic<uint32_t> sCompactCount;

    void ForestDatabase::compact() {
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
    fdb_compact_decision ForestDatabase::compactionCallback(fdb_file_handle *fhandle,
                                                      fdb_compaction_status status,
                                                      const char *kv_store_name,
                                                      fdb_doc *doc,
                                                      uint64_t last_oldfile_offset,
                                                      uint64_t last_newfile_offset,
                                                      void *ctx)
    {
        if (((ForestDatabase*)ctx)->onCompact(status, kv_store_name, doc,
                                              last_oldfile_offset, last_newfile_offset))
            return FDB_CS_KEEP_DOC;
        else
            return FDB_CS_DROP_DOC;
    }

    bool ForestDatabase::onCompact(fdb_compaction_status status,
                             const char *kv_store_name,
                             fdb_doc *doc,
                             uint64_t last_oldfile_offset,
                             uint64_t last_newfile_offset)
    {
        switch (status) {
            case FDB_CS_BEGIN:
                _isCompacting = true;
                ++sCompactCount;
                Log("ForestDatabase %p COMPACTING...", this);
                break;
            case FDB_CS_COMPLETE:
                updatePurgeCount();
                _isCompacting = false;
                --sCompactCount;
                Log("ForestDatabase %p END COMPACTING", this);
                break;
            default:
                return true; // skip the onCompactCallback
        }
        if (_onCompactCallback)
            _onCompactCallback(_isCompacting);
        return true;
    }

    bool ForestDatabase::isAnyCompacting() {
        return sCompactCount > 0;
    }

    void ForestDatabase::setAutoCompact(bool autoCompact) {
        auto mode = (autoCompact ? FDB_COMPACTION_AUTO : FDB_COMPACTION_MANUAL);
        check(fdb_switch_compaction_mode(_fileHandle, mode, _config.compaction_threshold));
        _config.compaction_mode = mode;
    }


#pragma mark - KEY-STORES:


    KeyStore* ForestDatabase::newKeyStore(const string &name, KeyStore::Options options) {
        return new ForestKeyStore(*this, name, options);
    }

    void ForestDatabase::deleteKeyStore(const string &name) {
        check(fdb_kvs_remove(_fileHandle, name.c_str()));
    }


    vector<string> ForestDatabase::allKeyStoreNames() {
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


    ForestKeyStore::ForestKeyStore(ForestDatabase &db, const string &name, KeyStore::Options options)
    :KeyStore(db, name, options)
    {
        reopen();
    }

    ForestKeyStore::~ForestKeyStore() {
        if (_handle)
            fdb_kvs_close(_handle);
    }

    void ForestKeyStore::reopen() {
        if (!_handle) {
            auto &db = (ForestDatabase&)database();
            check(fdb_kvs_open(db._fileHandle, &_handle, name().c_str(),  NULL));
            (void)fdb_set_log_callback(_handle, logCallback, _handle);
        }
    }


    void ForestKeyStore::close() {
        if (_handle) {
            check(fdb_kvs_close(_handle));
            _handle = nullptr;
        }
    }


    uint64_t ForestKeyStore::documentCount() const {
        fdb_kvs_info info;
        check(fdb_get_kvs_info(_handle, &info));
        return info.doc_count;
    }


    sequence ForestKeyStore::lastSequence() const {
        fdb_seqnum_t seq;
        check(fdb_get_kvs_seqnum(_handle, &seq));
        return seq;
    }


    void ForestKeyStore::setDocNoKey(Document &doc, fdb_doc &fdoc) const {
        doc.adoptMeta(slice(fdoc.meta, fdoc.metalen));
        doc.adoptBody(slice(fdoc.body, fdoc.bodylen));
        doc.setDeleted(fdoc.deleted);
        updateDoc(doc, fdoc.seqnum, fdoc.offset, fdoc.deleted);
    }

    void ForestKeyStore::setDoc(Document &doc, fdb_doc &fdoc) const {
        doc.adoptKey(slice(fdoc.key, fdoc.keylen));
        setDocNoKey(doc, fdoc);
    }


    bool ForestKeyStore::read(Document &doc, ContentOptions options) const {
        fdb_doc fdoc = {};
        fdoc.key = (void*)doc.key().buf;
        fdoc.keylen = doc.key().size;
        fdb_status status;
        if (options & kMetaOnly)
            status = fdb_get_metaonly(_handle, &fdoc);
        else
            status = fdb_get(_handle, &fdoc);
        if (!checkGet(status))
            return false;
        setDocNoKey(doc, fdoc);
        // (the heap blocks pointed to by fdoc have been adopted by doc, so don't free them.)
        return true;
    }


    void ForestKeyStore::readBody(Document &doc) const {
        if (doc.offset() > 0) {
            slice existingKey = doc.key();
            fdb_doc fdoc = {};
            fdoc.offset = doc.offset();
            fdoc.key = (void*)existingKey.buf;
            fdoc.keylen = existingKey.size;
            fdoc.seqnum = doc.sequence() ?: SEQNUM_NOT_USED;
            
            check(fdb_get_byoffset(_handle, &fdoc));

            doc.adoptBody(slice(fdoc.body, fdoc.bodylen));
            if (fdoc.key != existingKey.buf)
                free(fdoc.key);
            free(fdoc.meta);
        } else {
            KeyStore::readBody(doc);
        }
    }


    Document ForestKeyStore::get(sequence seq, ContentOptions options) const {
        Document doc;
        fdb_doc fdoc = {};
        fdoc.seqnum = seq;
        fdb_status status;
        if (options & kMetaOnly)
            status = fdb_get_metaonly_byseq(_handle, &fdoc);
        else
            status = fdb_get_byseq(_handle, &fdoc);
        if (checkGet(status)) {
            setDoc(doc, fdoc);
        }
        // (the heap blocks pointed to by fdoc have been adopted by doc, so don't free them.)
        return doc;
    }


    Document ForestKeyStore::getByOffsetNoErrors(uint64_t offset, sequence seq) const {
        Document result;

        fdb_doc fdoc = {};
        fdoc.offset = offset;
        fdoc.seqnum = seq;
        if (fdb_get_byoffset(_handle, &fdoc) == FDB_RESULT_SUCCESS)
            setDoc(result, fdoc);
        return result;
    }



    sequence ForestKeyStore::set(slice key, slice meta, slice body, Transaction&) {
        fdb_doc fdoc = {
            key.size, meta.size, body.size, 0,
            (void*)key.buf,
            0, 0,
            (void*)meta.buf, (void*)body.buf,
            false,
            0
        };
        check(fdb_set(_handle, &fdoc));
        return fdoc.seqnum;
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


    class ForestEnumerator : public DocEnumerator::Impl {
    public:
        ForestEnumerator(ForestKeyStore &store, fdb_iterator *iterator, DocEnumerator::Options &options)
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
            return true;    // iterator is already positioned at first doc when created
        }


        bool next() override {
            auto fn = _descending ? fdb_iterator_prev : fdb_iterator_next;
            auto status = fn(_iterator);
            if (status == FDB_RESULT_ITERATOR_FAIL)
                return false;
            check(status);
            return true;
        }

        bool seek(slice key) override {
            fdb_status status = fdb_iterator_seek(_iterator, key.buf, key.size,
                                                  (_descending ? FDB_ITR_SEEK_LOWER
                                                   : FDB_ITR_SEEK_HIGHER));
            if (status == FDB_RESULT_ITERATOR_FAIL)
                return false;
            check(status);
            return true;
        }

        bool read(Document &doc) override {
            auto fn = (_metaOnly ? fdb_iterator_get_metaonly : fdb_iterator_get);
            fdb_doc fdoc = { };
            fdb_doc *docP = &fdoc;
            fdb_status status = fn(_iterator, &docP);
            if (status == FDB_RESULT_ITERATOR_FAIL)
                return false;
            check(status);
            _store.setDoc(doc, fdoc);
            Debug("enum:     fdb_iterator_get --> [%s]", doc.key().hexCString());
            return true;
        }

    private:
        ForestKeyStore &_store;
        fdb_iterator *_iterator {nullptr};
        bool _descending;
        bool _metaOnly;
    };



    static fdb_iterator_opt_t iteratorOptions(const DocEnumerator::Options& options) {
        fdb_iterator_opt_t fdbOptions = 0;
        if (!options.includeDeleted)
            fdbOptions |= FDB_ITR_NO_DELETES;
        if (!options.inclusiveEnd)
            fdbOptions |= (options.descending ? FDB_ITR_SKIP_MIN_KEY : FDB_ITR_SKIP_MAX_KEY);
        if (!options.inclusiveStart)
            fdbOptions |= (options.descending ? FDB_ITR_SKIP_MAX_KEY : FDB_ITR_SKIP_MIN_KEY);
        return fdbOptions;
    }


    DocEnumerator::Impl* ForestKeyStore::newEnumeratorImpl(slice minKey, slice maxKey,
                                                     DocEnumerator::Options &options)
    {
        fdb_iterator *iterator;
        check(fdb_iterator_init(_handle, &iterator,
                                minKey.buf, minKey.size,
                                maxKey.buf, maxKey.size,
                                iteratorOptions(options)));
        return new ForestEnumerator(*this, iterator, options);
    }


    DocEnumerator::Impl* ForestKeyStore::newEnumeratorImpl(sequence minSeq, sequence maxSeq,
                                                     DocEnumerator::Options &options)
    {
        fdb_iterator *iterator;
        check(fdb_iterator_sequence_init(_handle, &iterator,
                                         minSeq, maxSeq,
                                         iteratorOptions(options)));
        return new ForestEnumerator(*this, iterator, options);
    }

}
