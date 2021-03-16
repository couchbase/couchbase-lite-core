//
//  Address.cc
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
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

#include "fleece/Fleece.hh"
#include "c4Database.h"
#include "c4Replicator.h"
#include "Address.hh"
#include "Error.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;
using namespace litecore;


namespace litecore { namespace net {

    Address::Address(const alloc_slice &url)
    :_url(url)
    {
        if (!c4address_fromURL(_url, this, nullptr))
            error::_throw(error::Network, kC4NetErrInvalidURL);
    }


    Address::Address(const C4Address &addr)
    :Address(alloc_slice(c4address_toURL(addr)))
    { }


    inline C4Address mkAddr(slice scheme, slice hostname, uint16_t port, slice uri) {
        C4Address address = {};
        address.scheme = scheme;
        address.hostname = hostname;
        address.port = port;
        address.path = uri;
        return address;
    }


    Address::Address(slice scheme, slice hostname, uint16_t port, slice uri)
    :Address(mkAddr(scheme, hostname, port, uri))
    { }


    Address& Address::operator= (const Address &other) {
        *((C4Address*)this) = other;
        _url = other._url;
        return *this;
    }


    static alloc_slice dbURL(C4Database *db) {
        alloc_slice path(c4db_getPath(db));
        return alloc_slice(string("file:///") + string(path));
    }


    Address::Address(C4Database *db)
    :Address( dbURL(db) )
    { }


    alloc_slice Address::toURL(const C4Address &c4Addr) noexcept {
        return c4address_toURL(c4Addr);
    }


    bool Address::isSecure(const C4Address &addr) noexcept {
        const C4Slice wss = kC4Replicator2TLSScheme;
        return (addr.scheme == wss || addr.scheme == "https"_sl);
    }

    bool Address::domainEquals(slice d1, slice d2) noexcept {
        return d1.caseEquivalent(d2);
    }

    bool Address::domainContains(slice baseDomain_, slice hostname_) noexcept {
        string baseDomain(baseDomain_), hostname(hostname_);
        return hasSuffixIgnoringCase(hostname, baseDomain)
            && (hostname.size() == baseDomain.size()
                || hostname[hostname.size() - baseDomain.size() - 1] == '.');
    }

    bool Address::pathContains(slice basePath, slice path) noexcept {
        if (basePath.size == 0)
            basePath = "/"_sl;
        if (path.size == 0)
            path = "/"_sl;
        return path.hasPrefix(basePath)
            && (path.size == basePath.size
                || path[basePath.size] == '/'
                || basePath[basePath.size-1] == '/');
    }

} }
