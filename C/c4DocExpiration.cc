//
// c4DocExpiration.cc
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
#include "c4Database.h"
#include "c4Document.h"

#include "Database.hh"
#include "KeyStore.hh"
#include "fleece/slice.hh"
#include <stdint.h>
#include <ctime>

using namespace fleece;


bool c4doc_setExpiration(C4Database *db, C4Slice docId, uint64_t timestamp, C4Error *outError) noexcept {
    if (!c4db_beginTransaction(db, outError))
        return false;
    bool ok = tryCatch<bool>(outError, [=]{
        if (db->defaultKeyStore().setExpiration(docId, timestamp))
            return true;
        recordError(LiteCoreDomain, kC4ErrorNotFound, outError);
        return false;
    });
    return c4db_endTransaction(db, ok, outError) && ok;
}


uint64_t c4doc_getExpiration(C4Database *db, C4Slice docID) noexcept {
    return tryCatch<uint64_t>(nullptr, [=]{
        return db->defaultKeyStore().getExpiration(docID);
    });
}


uint64_t c4db_nextDocExpiration(C4Database *db) noexcept
{
    return tryCatch<uint64_t>(nullptr, [=]{
        return db->defaultKeyStore().nextExpiration();
    });
}


int64_t c4db_purgeExpiredDocs(C4Database *db, C4Error *outError) noexcept
{
    int64_t count = -1;
    if (c4db_beginTransaction(db, outError)) {
        try {
            count = db->defaultKeyStore().expireRecords();
        } catchError(outError);
        if (!c4db_endTransaction(db, (count > 0), outError))
            count = -1;
    }
    return count;
}
