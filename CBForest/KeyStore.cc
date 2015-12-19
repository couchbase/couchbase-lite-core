//
//  KeyStore.cc
//  CBForest
//
//  Created by Jens Alfke on 11/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "KeyStore.hh"
#include "Document.hh"
#include "LogInternal.hh"

namespace cbforest {

    KeyStore::KeyStore(const Database* db, std::string name)
    :_handle(db->openKVS(name))
    { }

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

    void KeyStore::deleteKeyStore(Transaction& trans, bool recreate) {
        std::string name = this->name();
        trans.database()->deleteKeyStore(name);
        _handle = NULL;
        if (recreate)
            _handle = trans.database()->openKVS(name);
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
