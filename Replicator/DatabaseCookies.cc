//
//  DatabaseCookies.cc
//  LiteCore
//
//  Created by Jens Alfke on 6/9/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DatabaseCookies.hh"
#include "CookieStore.hh"
#include "Database.hh"

using namespace std;
using namespace fleece;
using namespace litecore::websocket;

namespace litecore { namespace repl {

    static const char *kInfoKeyStore = "info";
    static constexpr slice kCookieStoreDocID = "org.couchbase.cookies"_sl;


    DatabaseCookies::DatabaseCookies(c4Database *db)
    :_db(db)
    {
        auto object = db->dataFile()->sharedObject("CookieStore");
        if (!object) {
            alloc_slice data = _db->getRawDocument(kInfoKeyStore, kCookieStoreDocID).body();
            object = db->dataFile()->addSharedObject("CookieStore", new CookieStore(data));
        }
        _store = (CookieStore*)object.get();
    }


    void DatabaseCookies::saveChanges() {
        if (!_store->changed())
            return;
        _db->beginTransaction();
        try {
            alloc_slice data = _store->encode();
            _db->putRawDocument(kInfoKeyStore, kCookieStoreDocID, nullslice, data);
            _store->clearChanged();
            _db->endTransaction(true);
        } catch (...) {
            _db->endTransaction(false);
            throw;
        }
    }

} }
