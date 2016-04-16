//
//  c4ExpiryEnumerator.c
//  CBForest
//
//  Created by Jim Borden on 4/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4ExpiryEnumerator.h"

#include "DocEnumerator.hh"
#include "KeyStore.hh"
#include "varint.hh"
#include "stdint.h"
using namespace cbforest;

struct C4ExpiryEnumerator
{
public:
    C4ExpiryEnumerator(C4Database *database) :
    _db(database),
    _reader(slice::null)
    {
        _endTimestamp = time(NULL);
        reset();
    }
    
    bool next() {
        if(!_e.next()) {
            return false;
        }
        
        _reader = CollatableReader(_e.doc().key());
        _reader.skipTag();
        _reader.readInt();
        _current = _reader.readString();
        
        return true;
    }
    
    slice docID() const
    {
        return _current;
    }
    
    slice key() const
    {
        return _e.doc().key();
    }
    
    void reset()
    {
        CollatableBuilder c;
        c.beginArray();
        c << _endTimestamp;
        c.beginMap();
        c.endMap();
        c.endArray();
        _e = DocEnumerator(KeyStore((const Database*)_db, "expiry"), slice::null, c.data());
        _reader = CollatableReader(slice::null);
    }
    
    C4Database *getDatabase() const
    {
        return _db;
    }
    
private:
    C4Database *_db;
    DocEnumerator _e;
    alloc_slice _current;
    CollatableReader _reader;
    uint64_t _endTimestamp;
};

C4ExpiryEnumerator *c4db_enumerateExpired(C4Database *database, C4Error *outError)
{
    WITH_LOCK(database);
    return new C4ExpiryEnumerator(database);
}

bool c4exp_next(C4ExpiryEnumerator *e, C4Error *outError)
{
    try {
        return e->next();
    } catchError(outError);
    
    return false;
}

void c4exp_getInfo(C4ExpiryEnumerator *e, C4DocumentInfo *docInfo)
{
    auto d = e->docID();
    docInfo->docID = {d.buf, d.size};
    docInfo->sequence = 0ul;
    docInfo->revID = kC4SliceNull;
    docInfo->flags = 0;
}

bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError)
{
    try {
        e->reset();
        Transaction t(e->getDatabase());
        KeyStore expiry(e->getDatabase(), "expiry");
        KeyStoreWriter writer = t(expiry);
        while(e->next()) {
            writer.del(e->key());
            writer.del(e->docID());
        }
        
        return true;
    } catchError(outError);
    
    return false;
}

void c4exp_free(C4ExpiryEnumerator *e)
{
    delete e;
}