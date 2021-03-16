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
#include "c4Internal.hh"
#include "DatabaseImpl.hh"
#include "DatabaseCookies.hh"
#include "Error.hh"
#include "CookieStore.hh"

using namespace std;
using namespace fleece;

namespace litecore { namespace repl {

    static const char *kInfoKeyStore = "info";
    static constexpr slice kCookieStoreDocID = "org.couchbase.cookies"_sl;


    DatabaseCookies::DatabaseCookies(C4Database *db)
    :_db(db)
    {
        auto dataFile = asInternal(db)->dataFile();
        auto object = dataFile->sharedObject("CookieStore");
        if (!object) {
            alloc_slice data;
            _db->getRawDocument(kInfoKeyStore, kCookieStoreDocID, [&](C4RawDocument *doc) {
                slice cookies = doc ? doc->body : nullslice;
                object = dataFile->addSharedObject("CookieStore", new net::CookieStore(cookies));
            });
            DebugAssert(object);
        }
        _store = dynamic_cast<net::CookieStore*>(object.get());
    }


    void DatabaseCookies::saveChanges() {
        if (!_store->changed())
            return;
        C4Database::Transaction t(_db);
        _db->putRawDocument(kInfoKeyStore, {kCookieStoreDocID, nullslice, _store->encode()});
        t.commit();
        _store->clearChanged();
    }

} }
