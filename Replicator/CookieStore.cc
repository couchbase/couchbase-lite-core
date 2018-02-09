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
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "PlatformCompat.hh"
#include <iterator>
#include <regex>
#include <string>
#include <time.h>

#ifdef _MSC_VER
#include "strptime.h"
#endif

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore::websocket;

namespace litecore { namespace repl {

    static void offset_to_gmt(struct tm* inputTime)
    {
        // Get the raw time_t from the local time
        time_t rawtime = mktime(inputTime);
        struct tm* ptm;

        // Convert that raw time_t to GMT
#if !defined(WIN32)
        struct tm gbuf;
        ptm = gmtime_r(&rawtime, &gbuf);
#else
        ptm = gmtime(&rawtime);
#endif

        // Caveat: Undefined behavior during ambigiuous times (e.g. during a repeated
        // hour at the end of daylight savings time)
        time_t gmt = mktime(ptm);

        // Offset the original time by the difference that was calculated
        inputTime->tm_sec += difftime(rawtime, gmt);
    }

    static time_t parse_gmt_time(const char* timeStr)
    {
        struct tm datetime = { 0 };
        if (strptime(timeStr, "%a, %d %b %Y %T", &datetime) == nullptr) {
            Warn("Couldn't parse Expires in cookie");
            return 0;
        }

        offset_to_gmt(&datetime);
        return mktime(&datetime);
    }

#pragma mark - COOKIE:


    Cookie::Cookie(const string &header, const string &fromHost)
    :domain(fromHost)
    ,created(time(nullptr))
    {
        // <https://tools.ietf.org/html/rfc6265#section-4.1.1>
        static const regex sCookieRE("\\s*([^;=]+)=([^;=]*)");
        sregex_iterator match(header.begin(), header.end(), sCookieRE);
        sregex_iterator end;
        string provisionalName;
        int i = 0;
        for (; match != end; ++match, ++i) {
            string key = (*match)[1];
            string val = (*match)[2];
            if (i == 0) {
                provisionalName = key;
                if (hasPrefix(val,"\"") && hasSuffix(val, "\""))
                    val = val.substr(1, val.size()-2);
                value = val;
            } else if (key == "Domain") {
                if (!Address::domainContains(fromHost, val)) {
                    Warn("Cookie Domain isn't legal");
                    return;
                }
                domain = val;
            } else if (key == "Path") {
                path = val;
            } else if (key == "Secure") {
                secure = true;
            } else if (key == "Expires") {
                if (expires == 0) {
                    expires = parse_gmt_time(val.c_str());
                    if(expires == 0) {
                        return;
                    }
                }
            } else if (key == "Max-Age") {
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


    bool Cookie::matches(const websocket::Address &addr) const {
        return Address::domainContains(domain, addr.hostname)
            && Address::pathContains(path, addr.path)
            && (!secure || addr.isSecure());
    }


    ostream& operator<< (ostream &out, const Cookie &cookie) {
        return out << cookie.name << '=' << cookie.value;
    }

    fleeceapi::Encoder& operator<< (fleeceapi::Encoder &enc, const Cookie &cookie) {
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
        lock_guard<mutex> lock(const_cast<mutex&>(_mutex));
        vector<const Cookie*> cookies;
        cookies.reserve(_cookies.size());
        for (const CookiePtr &cookie : _cookies)
            cookies.push_back(cookie.get());
        return cookies;
    }


    string CookieStore::cookiesForRequest(const websocket::Address &addr) const {
        lock_guard<mutex> lock(const_cast<mutex&>(_mutex));
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


    bool CookieStore::setCookie(const string &headerValue, const string &fromHost) {
        auto newCookie = make_unique<const Cookie>(headerValue, fromHost);
        if (!newCookie->valid())
            return false;
        lock_guard<mutex> lock(const_cast<mutex&>(_mutex));
        _addCookie(move(newCookie));
        return true;
    }


    void CookieStore::merge(slice data) {
        CookieStore other(data);
        lock_guard<mutex> lock(const_cast<mutex&>(_mutex));
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
        lock_guard<mutex> lock(const_cast<mutex&>(_mutex));
        for (auto i = _cookies.begin(); !_changed && i != _cookies.end(); ++i) {
            if ((*i)->persistent())
                _changed = true;
        }
        _cookies.clear();
    }


    bool CookieStore::changed() {
        lock_guard<mutex> lock(const_cast<mutex&>(_mutex));
        return _changed;
    }

    void CookieStore::clearChanged() {
        lock_guard<mutex> lock(const_cast<mutex&>(_mutex));
        _changed = false;
    }

} }
