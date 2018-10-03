//
// CookieStore.hh
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
#include "RefCounted.hh"
#include "fleece/Fleece.hh"
#include "c4Replicator.h" // for C4Address
#include <iostream>
#include <memory>
#include <mutex>
#include <time.h>
#include <vector>

namespace litecore { namespace repl {
    using fleece::RefCounted;
    using fleece::Retained;

    /** Represents an HTTP cookie. */
    struct Cookie {
        // The constructors won't throw exceptions on invalid input, but the resulting Cookie
        // will return false from valid().

        Cookie() =default;
        Cookie(const std::string &header, const std::string &fromHost, const std::string &fromPath);
        Cookie(fleece::Dict);

        explicit operator bool() const  {return valid();}
        bool valid() const              {return !name.empty();}
        bool persistent() const         {return expires > 0;}
        bool expired() const            {return expires > 0 && expires < time(NULL);}

        bool matches(const Cookie&) const;
        bool matches(const C4Address&) const;
        bool sameValueAs(const Cookie&) const;

        std::string name;
        std::string value;
        std::string domain;
        std::string path;
        time_t created;
        time_t expires      {0};
        bool secure         {false};
    };

    std::ostream& operator<< (std::ostream&, const Cookie&);
    fleece::Encoder& operator<< (fleece::Encoder&, const Cookie&);


    /** Stores cookies, with support for persistent storage.
        Cookies are added from Set-Cookie headers, and the instance can generate Cookie: headers to
        send in requests.
        Instances are thread-safe. */
    class CookieStore : public RefCounted {
    public:
        CookieStore() =default;
        CookieStore(fleece::slice data);

        fleece::alloc_slice encode();

        std::vector<const Cookie*> cookies() const;
        std::string cookiesForRequest(const C4Address&) const;

        // Adds a cookie from a Set-Cookie: header value. Returns false if cookie is invalid.
        bool setCookie(const std::string &headerValue,
                       const std::string &fromHost,
                       const std::string &fromPath);
        
        void clearCookies();

        void merge(fleece::slice data);

        bool changed();
        void clearChanged();

    private:
        using CookiePtr = std::unique_ptr<const Cookie>;

        void _addCookie(CookiePtr newCookie);

        CookieStore(const CookieStore&) =delete;

        std::vector<CookiePtr> _cookies;
        bool _changed {false};
        std::mutex _mutex;
    };

} }
