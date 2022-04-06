//
// CookieStore.hh
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
#include "fleece/RefCounted.hh"
#include "fleece/Fleece.hh"
#include <iosfwd>
#include <memory>
#include <mutex>
#include <time.h>
#include <vector>

struct C4Address;

namespace litecore { namespace net {
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
        explicit CookieStore(fleece::slice data);

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
        mutable std::mutex _mutex;
    };

} }
