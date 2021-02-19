//
// DatabaseCookies.hh
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

#pragma once
#include "CookieStore.hh"

struct C4Database;

namespace litecore { namespace repl {
    using namespace fleece;

    /** Persists a CookieStore to/from a Database. */
    class DatabaseCookies {
    public:
        DatabaseCookies(C4Database*);

        std::string cookiesForRequest(const C4Address &addr) {
            return _store->cookiesForRequest(addr);
        }

        // Adds a cookie from a Set-Cookie: header value. Returns false if cookie is invalid.
        bool setCookie(const std::string &headerValue,
                       const std::string &fromHost,
                       const std::string &fromPath)
        {
            return _store->setCookie(headerValue, fromHost, fromPath);
        }

        void clearCookies() {
            _store->clearCookies();
        }

        void saveChanges();

    private:
        C4Database* _db;
        Retained<net::CookieStore> _store;
    };

} }

