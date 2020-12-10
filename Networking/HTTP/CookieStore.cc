//
// CookieStore.cc
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

#include "CookieStore.hh"
#include "Address.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "PlatformCompat.hh"
#include <iterator>
#include <regex>
#include <string>
#include <time.h>
#include <chrono>
#include <algorithm>
#include "date/date.h"
#include "ParseDate.hh"

using namespace std;
using namespace std::chrono;
using namespace date;
using namespace fleece;
using namespace litecore::net;

namespace litecore { namespace net {

    static time_t parse_gmt_time(const char* timeStr)
    {
        istringstream s(timeStr);
        sys_seconds tp;
        s >> parse("%a, %d %b %Y %T GMT", tp);
        if (s.fail()) {
            Warn("Couldn't parse Expires in cookie");
            return 0;
        }

        // The limit of 32-bit time_t is approaching...
        auto result = tp.time_since_epoch().count();
        if(_usuallyFalse(result > numeric_limits<time_t>::max())) {
            Warn("time_t overflow, capping to max!");
            return numeric_limits<time_t>::max();
        }
        
        if(_usuallyFalse(result < numeric_limits<time_t>::min())) {
            Warn("time_t underflow, capping to min!");
            return numeric_limits<time_t>::min();
        }
        
        return (time_t)result;
    }

#pragma mark - COOKIE:


    Cookie::Cookie(const string &header, const string &fromHost, const string &fromPath)
    :domain(fromHost)
    ,created(time(nullptr))
    {
        // Default path is the request path minus its last component:
        auto slash = fromPath.rfind('/');
        if (slash != string::npos && slash > 0)
            path = fromPath.substr(0, slash);

        // <https://tools.ietf.org/html/rfc6265#section-4.1.1>
        static const regex sCookieRE("\\s*([^;=]+)=([^;=]*)");
        sregex_iterator match(header.begin(), header.end(), sCookieRE);
        sregex_iterator end;
        string provisionalName;
        int i = 0;
        for (; match != end; ++match, ++i) {
            string key = lowercase((*match)[1]);
            string val = (*match)[2];
            if (i == 0) {
                provisionalName = key;
                if (hasPrefix(val,"\"") && hasSuffix(val, "\""))
                    val = val.substr(1, val.size()-2);
                value = val;
            } else if (key == "domain") {
                while (!val.empty() && val[0] == '.')
                    val.erase(0, 1);
                if (!Address::domainContains(fromHost, val)) {
                    Warn("Cookie Domain isn't legal");
                    return;
                }
                domain = val;
            } else if (key == "path") {
                path = val;
            } else if (key == "secure") {
                secure = true;
            } else if (key == "expires") {
                if (expires == 0) {
                    expires = parse_gmt_time(val.c_str());
                    if(expires == 0) {
                        return;
                    }
                }
            } else if (key == "max-age") {
                char *valEnd = &val[val.size()];
                long maxAge = strtol(&val[0], &valEnd, 10);
                if (valEnd != &val[val.size()] || val.size() == 0) {
                    Warn("Couldn't parse Max-Age in cookie");
                    return;
                }
                expires = created + maxAge;
            }
        }

        if (i == 0) {
            Warn("Couldn't parse Set-Cookie header: %s", header.c_str());
            return;
        }
        name = provisionalName;
    }


    Cookie::Cookie(Dict dict)
    :name(dict["name"].asstring())
    ,value(dict["value"].asstring())
    ,domain(dict["domain"].asstring())
    ,path(dict["path"].asstring())
    ,created((time_t)dict["created"].asInt())
    ,expires((time_t)dict["expires"].asInt())
    ,secure(dict["secure"].asBool())
    {
        if (domain.empty() || expires == 0 || created == 0)
            name.clear();  // invalidate
    }


    bool Cookie::matches(const Cookie &c) const {
        return name == c.name && compareIgnoringCase(domain, c.domain) == 0 && path == c.path;
    }


    bool Cookie::sameValueAs(const Cookie &c) const {
        return value == c.value && expires == c.expires && secure == c.secure;
    }


    bool Cookie::matches(const C4Address &addr) const {
        return Address::domainContains(domain, addr.hostname)
            && Address::pathContains(path, addr.path)
            && (!secure || Address::isSecure(addr));
    }


    ostream& operator<< (ostream &out, const Cookie &cookie) {
        return out << cookie.name << '=' << cookie.value;
    }

    fleece::Encoder& operator<< (fleece::Encoder &enc, const Cookie &cookie) {
        Assert(cookie.persistent());
        enc.beginDict(6);
        enc.writeKey("name"_sl);
        enc.writeString(cookie.name);
        enc.writeKey("value"_sl);
        enc.writeString(cookie.value);
        enc.writeKey("domain"_sl);
        enc.writeString(cookie.domain);
        enc.writeKey("created"_sl);
        enc.writeInt(cookie.created);
        enc.writeKey("expires"_sl);
        enc.writeInt(cookie.expires);
        if (!cookie.path.empty()) {
            enc.writeKey("path"_sl);
            enc.writeString(cookie.path);
        }
        if (cookie.secure) {
            enc.writeKey("secure"_sl);
            enc.writeBool(true);
        }
        enc.endDict();
        return enc;
    }


#pragma mark - COOKIE STORE:


    CookieStore::CookieStore(slice data) {
        if (data.size == 0)
            return;
        Array cookies = Value::fromData(data).asArray();
        if (!cookies) {
            Warn("Couldn't parse persisted cookie store!");
            return;
        }
        for (Array::iterator i(cookies); i; ++i) {
            auto cookie = make_unique<const Cookie>(i.value().asDict());
            if (cookie->valid()) {
                if (!cookie->expired())
                    _cookies.emplace_back(move(cookie));
            } else {
                Warn("Couldn't read a cookie from persisted cookie store!");
            }
        }
    }


    alloc_slice CookieStore::encode() {
        lock_guard<mutex> lock(_mutex);
        Encoder enc;
        enc.beginArray(_cookies.size());
        for (CookiePtr &cookie : _cookies) {
            if (cookie->persistent() && !cookie->expired())
                enc << *cookie;
        }
        enc.endArray();
        return enc.finish();
    }


    vector<const Cookie*> CookieStore::cookies() const {
        lock_guard<mutex> lock(_mutex);
        vector<const Cookie*> cookies;
        cookies.reserve(_cookies.size());
        for (const CookiePtr &cookie : _cookies)
            cookies.push_back(cookie.get());
        return cookies;
    }


    string CookieStore::cookiesForRequest(const C4Address &addr) const {
        lock_guard<mutex> lock(_mutex);
        stringstream s;
        int n = 0;
        for (const CookiePtr &cookie : _cookies) {
            if (cookie->matches(addr) && !cookie->expired()) {
                if (n++)
                    s << "; ";
                s << *cookie;
            }
        }
        return s.str();
    }


    bool CookieStore::setCookie(const string &headerValue,
                                const string &fromHost,
                                const string &path)
    {
        auto newCookie = make_unique<const Cookie>(headerValue, fromHost, path);
        if (!newCookie->valid())
            return false;
        lock_guard<mutex> lock(_mutex);
        _addCookie(move(newCookie));
        return true;
    }


    void CookieStore::merge(slice data) {
        CookieStore other(data);
        lock_guard<mutex> lock(_mutex);
        for (CookiePtr &cookie : other._cookies)
            _addCookie(move(cookie));
    }


    void CookieStore::_addCookie(CookiePtr newCookie) {
        for (auto i = _cookies.begin(); i != _cookies.end(); ++i) {
            const Cookie *oldCookie = i->get();
            if (newCookie->matches(*oldCookie)) {
                if (newCookie->created < oldCookie->created)
                    return;   // obsolete
                if (newCookie->sameValueAs(*oldCookie))
                    return;   // No-op
                // Remove the replaced cookie:
                if (oldCookie->persistent())
                    _changed = true;
                _cookies.erase(i);
                break;
            }
        }
        // Add the new cookie:
        if (newCookie->persistent())
            _changed = true;
        _cookies.emplace_back(move(newCookie));
    }


    void CookieStore::clearCookies() {
        lock_guard<mutex> lock(_mutex);
        for (auto i = _cookies.begin(); !_changed && i != _cookies.end(); ++i) {
            if ((*i)->persistent())
                _changed = true;
        }
        _cookies.clear();
    }


    bool CookieStore::changed() {
        lock_guard<mutex> lock(_mutex);
        return _changed;
    }

    void CookieStore::clearChanged() {
        lock_guard<mutex> lock(_mutex);
        _changed = false;
    }

} }
