//
// DatabaseCookies.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "c4Database.hh"
#include "DatabaseCookies.hh"
#include "CookieStore.hh"

using namespace std;
using namespace fleece;

namespace litecore { namespace repl {

    static const char *kInfoKeyStore = "info";
    static constexpr slice kCookieStoreDocID = "org.couchbase.cookies"_sl;


    DatabaseCookies::DatabaseCookies(C4Database *db)
    :_db(db)
    {
        auto object = db->dataFile()->sharedObject("CookieStore");
        if (!object) {
            alloc_slice data = _db->getRawDocument(kInfoKeyStore, kCookieStoreDocID).body();
            object = db->dataFile()->addSharedObject("CookieStore", new net::CookieStore(data));
        }
        _store = dynamic_cast<net::CookieStore*>(object.get());
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
