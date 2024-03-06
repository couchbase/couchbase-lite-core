//
// DatabaseCookies.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Database.hh"
#include "DatabaseImpl.hh"
#include "DatabaseCookies.hh"
#include "Error.hh"
#include "CookieStore.hh"

using namespace std;
using namespace fleece;

namespace litecore::repl {

    static const char*     kInfoKeyStore     = "info";
    static constexpr slice kCookieStoreDocID = "org.couchbase.cookies"_sl;

    DatabaseCookies::DatabaseCookies(C4Database* db) : _db(db) {
        auto dataFile = asInternal(db)->dataFile();
        auto object   = dataFile->sharedObject("CookieStore");
        if ( !object ) {
            alloc_slice data;
            _db->getRawDocument(kInfoKeyStore, kCookieStoreDocID, [&](C4RawDocument* doc) {
                slice cookies = doc ? doc->body : nullslice;
                object        = dataFile->addSharedObject("CookieStore", new net::CookieStore(cookies));
            });
            DebugAssert(object);
        }
        _store = dynamic_cast<net::CookieStore*>(object.get());
    }

    void DatabaseCookies::saveChanges() {
        if ( !_store->changed() ) return;
        C4Database::Transaction t(_db);
        _db->putRawDocument(kInfoKeyStore, {kCookieStoreDocID, nullslice, _store->encode()});
        t.commit();
        _store->clearChanged();
    }

}  // namespace litecore::repl
