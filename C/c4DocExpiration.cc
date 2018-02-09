//
// c4ExpiryEnumerator.c
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4Internal.hh"
#include "c4ExpiryEnumerator.h"
#include "Database.hh"

#include "RecordEnumerator.hh"
#include "KeyStore.hh"
#include "varint.hh"
#include "slice.hh"
#include "FleeceCpp.hh"
#include <stdint.h>
#include <ctime>

using namespace fleece;
using namespace fleeceapi;


// This helper function is meant to be wrapped in a transaction
static bool c4doc_setExpirationInternal(C4Database *db, C4Slice docId, uint64_t timestamp, C4Error *outError)
{
    return tryCatch<bool>(outError, [&]{
        if (!db->defaultKeyStore().get(docId, kMetaOnly).exists()) {
            recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
            return false;
        }

        fleeceapi::Encoder enc;
        enc.beginArray();
        enc << (double)timestamp;
        enc << (slice)docId;
        enc.endArray();
        alloc_slice tsKey = enc.finish();

        alloc_slice tsValue(SizeOfVarInt(timestamp));
        PutUVarInt((void *)tsValue.buf, timestamp);

        Transaction &t = db->transaction();
        KeyStore& expiry = db->getKeyStore("expiry");
        Record existingDoc = expiry.get(docId);
        if (existingDoc.exists()) {
            // Previous entry found
            if (existingDoc.body().compare(tsValue) == 0) {
                // No change
                return true;
            }

            // Remove old entry
            uint64_t oldTimestamp = 0;
            fleeceapi::Encoder oldKeyEnc;
            GetUVarInt(existingDoc.body(), &oldTimestamp);
            oldKeyEnc.beginArray();
            oldKeyEnc << (double)oldTimestamp;
            oldKeyEnc << (slice)docId;
            oldKeyEnc.endArray();
            alloc_slice oldKey(oldKeyEnc.finish());
            expiry.del(oldKey, t);
        }

        if (timestamp == 0) {
            expiry.del(tsKey, t);
            expiry.del(docId, t);
        } else {
            expiry.set(tsKey, nullslice, t);
            expiry.set(docId, tsValue, t);
        }

        return true;
    });
}


bool c4doc_setExpiration(C4Database *db, C4Slice docId, uint64_t timestamp, C4Error *outError) noexcept {
    if (!c4db_beginTransaction(db, outError)) {
        return false;
    }

    bool commit = c4doc_setExpirationInternal(db, docId, timestamp, outError);
    return c4db_endTransaction(db, commit, outError);
}


uint64_t c4doc_getExpiration(C4Database *db, C4Slice docID) noexcept {
    KeyStore &expiryKvs = db->getKeyStore("expiry");
    Record existing = expiryKvs.get(docID);
    if (!existing.exists()) {
        return 0;
    }

    uint64_t timestamp = 0;
    GetUVarInt(existing.body(), &timestamp);
    return timestamp;
}


uint64_t c4db_nextDocExpiration(C4Database *database) noexcept
{
    return tryCatch<uint64_t>(nullptr, [database]{
        KeyStore& expiryKvs = database->getKeyStore("expiry");
        RecordEnumerator e(expiryKvs);
        if(e.next() && e.record().body() == nullslice) {
            // Look for an entry with a null body (otherwise, its key is simply a doc ID)
            Array info = Value::fromData(e.record().key()).asArray();
            return info[0U].asUnsigned();
        }
        return (uint64_t)0;
    });
}


#pragma mark - ENUMERATOR:


struct C4ExpiryEnumerator
{
public:
    C4ExpiryEnumerator(C4Database *database) :
    _db(database),
    _e(_db->getKeyStore("expiry"))
    
    {
        _endTimestamp = time(nullptr);
        reset();
    }

    bool next() {
        if(!_e.next()) {
            return false;
        }
        auto key = _e.record().key();
        if (key > _endKey) {
            return false;
        }
        auto info = Value::fromData(key).asArray();
        _current = alloc_slice(info[1U].asString());
        
        return true;
    }
    
    slice docID() const
    {
        return _current;
    }
    
    slice key() const
    {
        return _e.record().key();
    }
    
    void reset()
    {
        fleeceapi::Encoder e;
        e.beginArray();
        e << (double)_endTimestamp;
        e.beginDict();
        e.endDict();
        e.endArray();
        _endKey = e.finish();
        _e = RecordEnumerator(_db->getKeyStore("expiry"));
    }

    void close()
    {
        _e.close();
    }
    
    C4Database *getDatabase() const
    {
        return external(_db);
    }
    
private:
    Retained<Database> _db;
    RecordEnumerator _e;
    alloc_slice _current;
    uint64_t _endTimestamp;
    alloc_slice _endKey;
};

C4ExpiryEnumerator *c4db_enumerateExpired(C4Database *database, C4Error *outError) noexcept {
    return tryCatch<C4ExpiryEnumerator*>(outError, [&]{
        return new C4ExpiryEnumerator(database);
    });
}

bool c4exp_next(C4ExpiryEnumerator *e, C4Error *outError) noexcept {
    return tryCatch<bool>(outError, [&]{
        if (e->next())
            return true;
        clearError(outError);
        return false;
    });
}

C4SliceResult c4exp_getDocID(const C4ExpiryEnumerator *e) noexcept {
    return sliceResult(e->docID());
}

bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError) noexcept {
    if (!c4db_beginTransaction(e->getDatabase(), outError))
        return false;
    bool commit = tryCatch(outError, [&]{
        e->reset();
        Transaction &t = e->getDatabase()->transaction();
        KeyStore& expiry = e->getDatabase()->getKeyStore("expiry");
        while(e->next()) {
            expiry.del(e->key(), t);
            expiry.del(e->docID(), t);
        }
    });
    
    c4db_endTransaction(e->getDatabase(), commit,  nullptr);
    return commit;
}

void c4exp_close(C4ExpiryEnumerator *e) noexcept {
    if (e) {
        e->close();
    }
}

void c4exp_free(C4ExpiryEnumerator *e) noexcept {
    delete e;
}
