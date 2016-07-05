//
//  KeyStore.hh
//  CBForest
//
//  Created by Jens Alfke on 11/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__KeyStore__
#define __CBForest__KeyStore__

#include "Error.hh"
#include "forestdb.h"
#include "slice.hh"

namespace cbforest {

    class Database;
    class Document;
    class KeyStoreWriter;
    class Transaction;

    typedef fdb_seqnum_t sequence;

    /** Provides read-only access to a key-value store inside a Database.
        (Just a wrapper around a fdb_kvs_handle*.) */
    class KeyStore {
    public:
        typedef fdb_kvs_info kvinfo;

        void enableErrorLogs(bool enable);                  // defaults to true
        
        kvinfo getInfo() const;
        sequence lastSequence() const;
        std::string name() const;

        bool isOpen()                                       {return _handle != NULL;}

        // Keys/values:

        enum contentOptions {
            kDefaultContent = 0,
            kMetaOnly = 0x01
        };

        Document get(slice key, contentOptions = kDefaultContent) const;
        Document get(sequence, contentOptions = kDefaultContent) const;
        bool read(Document&, contentOptions = kDefaultContent) const; // key must already be set

        Document getByOffset(uint64_t offset, sequence) const;
        Document getByOffsetNoErrors(uint64_t offset, sequence) const;  // doesn't throw or log

        void close();
        void deleteKeyStore(Transaction& t);
        void erase();

    protected:
        KeyStore(fdb_kvs_handle* handle)                    :_handle(handle) { }
        fdb_kvs_handle* handle() const                      {return _handle;}

        fdb_kvs_handle* _handle;

    private:
        KeyStore(const KeyStore&) = delete;
        KeyStore& operator=(const KeyStore&) = delete;

        friend class Database;
        friend class DocEnumerator;
        friend class KeyStoreWriter;
    };


    /** Adds write access to a KeyStore. */
    class KeyStoreWriter : public KeyStore {
    public:
        KeyStoreWriter(const KeyStore &store, Transaction&) :KeyStore(store._handle) { }

        sequence set(slice key, slice meta, slice value);
        sequence set(slice key, slice value)                {return set(key, slice::null, value);}
        void write(Document&);

        bool del(slice key);
        bool del(sequence);
        bool del(Document&);

        void rollbackTo(sequence);

        friend class KeyStore;

        KeyStoreWriter(const KeyStoreWriter& k)            :KeyStore(k._handle) { }
        KeyStoreWriter& operator=(const KeyStoreWriter &k) {_handle = k._handle; return *this;}

    private:
        KeyStoreWriter(KeyStore& store)                      :KeyStore(store._handle) { }
        friend class Transaction;
        friend class Database;
    };

}

#endif /* defined(__CBForest__KeyStore__) */
