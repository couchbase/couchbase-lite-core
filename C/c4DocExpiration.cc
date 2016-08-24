//
//  c4ExpiryEnumerator.c
//  CBForest
//
//  Created by Jim Borden on 4/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "c4Internal.hh"
#include "c4ExpiryEnumerator.h"
#include "c4DatabaseInternal.hh"

#include "Collatable.hh"
#include "DocEnumerator.hh"
#include "KeyStore.hh"
#include "varint.hh"
#include <stdint.h>
#include <ctime>

using namespace fleece;


// This helper function is meant to be wrapped in a transaction
static bool c4doc_setExpirationInternal(C4Database *db, C4Slice docId, uint64_t timestamp, C4Error *outError)
{
    try {
        if (!db->defaultKeyStore().get(docId, kMetaOnly).exists()) {
            recordError(CBForestDomain, kC4ErrorNotFound, outError);
            return false;
        }

        CollatableBuilder tsKeyBuilder;
        tsKeyBuilder.beginArray();
        tsKeyBuilder << (double)timestamp;
        tsKeyBuilder << docId;
        tsKeyBuilder.endArray();
        slice tsKey = tsKeyBuilder.data();

        alloc_slice tsValue(SizeOfVarInt(timestamp));
        PutUVarInt((void *)tsValue.buf, timestamp);

        Transaction &t = db->transaction();
        KeyStore& expiry = db->getKeyStore("expiry");
        Document existingDoc = expiry.get(docId);
        if (existingDoc.exists()) {
            // Previous entry found
            if (existingDoc.body().compare(tsValue) == 0) {
                // No change
                return true;
            }

            // Remove old entry
            uint64_t oldTimestamp;
            CollatableBuilder oldTsKey;
            GetUVarInt(existingDoc.body(), &oldTimestamp);
            oldTsKey.beginArray();
            oldTsKey << (double)oldTimestamp;
            oldTsKey << docId;
            oldTsKey.endArray();
            expiry.del(oldTsKey, t);
        }

        if (timestamp == 0) {
            expiry.del(tsKey, t);
            expiry.del(docId, t);
        } else {
            expiry.set(tsKey, slice::null, t);
            expiry.set(docId, tsValue, t);
        }

        return true;
    } catchError(outError);

    return false;
}


bool c4doc_setExpiration(C4Database *db, C4Slice docId, uint64_t timestamp, C4Error *outError)
{
    if (!c4db_beginTransaction(db, outError)) {
        return false;
    }

    bool commit = c4doc_setExpirationInternal(db, docId, timestamp, outError);
    return c4db_endTransaction(db, commit, outError);
}


uint64_t c4doc_getExpiration(C4Database *db, C4Slice docID)
{
    KeyStore &expiryKvs = db->getKeyStore("expiry");
    Document existing = expiryKvs.get(docID);
    if (!existing.exists()) {
        return 0;
    }

    uint64_t timestamp;
    GetUVarInt(existing.body(), &timestamp);
    return timestamp;
}


#pragma mark - ENUMERATOR:


struct C4ExpiryEnumerator
{
public:
    C4ExpiryEnumerator(C4Database *database) :
    _db(database),
    _e(_db->getKeyStore("expiry"), slice::null, slice::null),
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
        c << (double)_endTimestamp;
        c.beginMap();
        c.endMap();
        c.endArray();
        _e = DocEnumerator(_db->getKeyStore("expiry"), slice::null, c.data());
        _reader = CollatableReader(slice::null);
    }

    void close()
    {
        _e.close();
    }
    
    C4Database *getDatabase() const
    {
        return _db;
    }
    
private:
    Retained<C4Database> _db;
    DocEnumerator _e;
    alloc_slice _current;
    CollatableReader _reader;
    uint64_t _endTimestamp;
};

C4ExpiryEnumerator *c4db_enumerateExpired(C4Database *database, C4Error *outError)
{
    try {
        WITH_LOCK(database);
        return new C4ExpiryEnumerator(database);
    } catchError(outError);

    return NULL;
}

bool c4exp_next(C4ExpiryEnumerator *e, C4Error *outError)
{
    try {
        if (e->next())
            return true;
        clearError(outError);
    } catchError(outError);
    return false;
}

C4SliceResult c4exp_getDocID(const C4ExpiryEnumerator *e)
{
    slice result = e->docID().copy();
    return { result.buf, result.size };
}

bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError)
{
    if (!c4db_beginTransaction(e->getDatabase(), outError))
        return false;
    bool commit = false;
    try {
        WITH_LOCK(e->getDatabase());
        e->reset();
        Transaction &t = e->getDatabase()->transaction();
        KeyStore& expiry = e->getDatabase()->getKeyStore("expiry");
        while(e->next()) {
            expiry.del(e->key(), t);
            expiry.del(e->docID(), t);
        }
        commit = true;
    } catchError(outError);
    
    c4db_endTransaction(e->getDatabase(), commit,  NULL);
    return commit;
}

void c4exp_close(C4ExpiryEnumerator *e)
{
    if (e) {
        e->close();
    }
}

void c4exp_free(C4ExpiryEnumerator *e)
{
    delete e;
}
