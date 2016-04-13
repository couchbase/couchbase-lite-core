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
    
    bool next(bool skipEmpty) {
        while(_reader.atEnd()) {
            if(!_e.next()) {
                return false;
            }
            
            _reader = CollatableReader(_e.doc().body());
            if(!skipEmpty) {
                break;
            }
        }
        
        if(!_reader.atEnd()) {
            _current = _reader.readString();
        } else {
            _current = slice::null;
        }
        
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
        c << _endTimestamp;
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
        return e->next(true);
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

void c4exp_free(C4ExpiryEnumerator *e, bool cleanupKvs)
{
    if(cleanupKvs) {
        e->reset();
        auto db = e->getDatabase();
        KeyStore kvs((const Database*)db, "expiry");
        Transaction t((Database *)db);
        KeyStoreWriter writer = t(kvs);
        while(e->next(false)) {
            slice docID = e->docID();
            if(docID != slice::null) {
                writer.del(e->docID());
            }
            
            writer.del(e->key());
        }
    }
    
    delete e;
}