//
//  KeyStore.cc
//  CBForest
//
//  Created by Jens Alfke on 11/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "KeyStore.hh"
#include "Document.hh"
#include "LogInternal.hh"

namespace cbforest {

    static void logCallback(int err_code, const char *err_msg, void *ctx_data) {
        WarnError("ForestDB error %d: %s (fdb_kvs_handle=%p)", err_code, err_msg, ctx_data);
    }

    static void nullLogCallback(int err_code, const char *err_msg, void *ctx_data) {
    }

    void KeyStore::enableErrorLogs(bool enable) {
        if (enable)
            (void)fdb_set_log_callback(_handle, logCallback, _handle);
        else
            (void)fdb_set_log_callback(_handle, nullLogCallback, NULL);
    }

    KeyStore::kvinfo KeyStore::getInfo() const {
        kvinfo i;
        check(fdb_get_kvs_info(_handle, &i));
        return i;
    }

    std::string KeyStore::name() const {
        return std::string(getInfo().name);
    }

    sequence KeyStore::lastSequence() const {
        fdb_seqnum_t seq;
        check(fdb_get_kvs_seqnum(_handle, &seq));
        return seq;
    }

    static bool checkGet(fdb_status status) {
        if (status == FDB_RESULT_KEY_NOT_FOUND)
            return false;
        check(status);
        return true;
    }

    Document KeyStore::get(slice key, contentOptions options) const {
        Document doc(key);
        read(doc, options);
        return doc;
    }

    Document KeyStore::get(sequence seq, contentOptions options) const {
        Document doc;
        doc._doc.seqnum = seq;
        if (options & kMetaOnly)
            check(fdb_get_metaonly_byseq(_handle, &doc._doc));
        else
            check(fdb_get_byseq(_handle, doc));
        return doc;
    }

    bool KeyStore::read(Document& doc, contentOptions options) const {
        doc.clearMetaAndBody();
        if (options & kMetaOnly)
            return checkGet(fdb_get_metaonly(_handle, doc));
        else
            return checkGet(fdb_get(_handle, doc));
    }

    Document KeyStore::getByOffset(uint64_t offset, sequence seq) const {
        Document doc;
        doc._doc.offset = offset;
        doc._doc.seqnum = seq;
        checkGet(fdb_get_byoffset(_handle, doc));
        return doc;
    }

    Document KeyStore::getByOffsetNoErrors(uint64_t offset, sequence seq) const {
        Document doc;
        doc._doc.offset = offset;
        doc._doc.seqnum = seq;

        const_cast<KeyStore*>(this)->enableErrorLogs(false);     // Don't log ForestDB errors caused by reading invalid offset
        (void) fdb_get_byoffset(_handle, doc);
        const_cast<KeyStore*>(this)->enableErrorLogs(true);

        return doc;
    }

    void KeyStore::close() {
        if (_handle) {
            fdb_kvs_close(_handle);
            _handle = NULL;
        }
    }

    void KeyStore::erase() {
        check(fdb_rollback(&_handle, 0));
    }

    void KeyStore::deleteKeyStore(Transaction& trans) {
        trans.database()->deleteKeyStore(name());
        _handle = NULL;
    }


#pragma mark - KEYSTOREWRITER:


    void KeyStoreWriter::rollbackTo(sequence seq) {
        check(fdb_rollback(&_handle, seq));
    }

    void KeyStoreWriter::write(Document &doc) {
        check(fdb_set(_handle, doc));
    }

    sequence KeyStoreWriter::set(slice key, slice meta, slice body) {
        if ((size_t)key.buf & 0x03) {
            // Workaround for unaligned-access crashes on ARM (down in forestdb's crc_32_8 fn)
            void* keybuf = alloca(key.size);
            memcpy(keybuf, key.buf, key.size);
            key.buf = keybuf;
        }
        fdb_doc doc = {};
        doc.key = (void*)key.buf;
        doc.keylen = key.size;
        doc.meta = (void*)meta.buf;
        doc.metalen = meta.size;
        doc.body = (void*)body.buf;
        doc.bodylen = body.size;

        check(fdb_set(_handle, &doc));
        Log("DB %p: added %s --> %s (meta %s) (seq %llu)\n",
            _handle, key.hexCString(), body.hexCString(), meta.hexCString(), doc.seqnum);
        return doc.seqnum;
    }

    bool KeyStoreWriter::del(cbforest::Document &doc) {
        return checkGet(fdb_del(_handle, doc));
    }

    bool KeyStoreWriter::del(cbforest::slice key) {
        if ((size_t)key.buf & 0x03) {
            // Workaround for unaligned-access crashes on ARM (down in forestdb's crc_32_8 fn)
            void* keybuf = alloca(key.size);
            memcpy(keybuf, key.buf, key.size);
            key.buf = keybuf;
        }
        fdb_doc doc = {};
        doc.key = (void*)key.buf;
        doc.keylen = key.size;

        return checkGet(fdb_del(_handle, &doc));
    }

    bool KeyStoreWriter::del(sequence seq) {
        Document doc;
        doc._doc.seqnum = seq;
        return checkGet(fdb_get_metaonly_byseq(_handle, doc))
            && del(doc);
    }

}
