//
//  DatabaseCookies.hh
//  LiteCore
//
//  Created by Jens Alfke on 6/9/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "CookieStore.hh"
#include "slice.hh"
#include <mutex>
#include <unordered_map>

class c4Database;

namespace litecore { namespace repl {
    class CookieStore;
    

    /** Persists a CookieStore to/from a Database. */
    class DatabaseCookies {
    public:
        DatabaseCookies(c4Database*);

        std::string cookiesForRequest(const websocket::Address &addr) {
            return _store->cookiesForRequest(addr);
        }

        bool setCookie(const std::string &headerValue, const std::string &fromHost) {
            return _store->setCookie(headerValue, fromHost);
        }

        void clearCookies() {
            _store->clearCookies();
        }

        void saveChanges();

    private:
        c4Database* _db;
        Retained<CookieStore> _store;
    };

} }

