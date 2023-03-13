//
// DatabaseCookies.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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

        std::string cookiesForRequest(const C4Address& addr) { return _store->cookiesForRequest(addr); }

        // Adds a cookie from a Set-Cookie: header value. Returns false if cookie is invalid.
        bool setCookie(const std::string& headerValue, const std::string& fromHost, const std::string& fromPath,
                       bool acceptParentDomain = false) {
            return _store->setCookie(headerValue, fromHost, fromPath, acceptParentDomain);
        }

        void clearCookies() { _store->clearCookies(); }

        void saveChanges();

      private:
        C4Database*                _db;
        Retained<net::CookieStore> _store;
    };

}}  // namespace litecore::repl
