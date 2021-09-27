//
//  Address.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// NOTE: This class is used in the C4Tests, which link against the LiteCore DLL,
// so it cannot use the LiteCore C++ API.

#include "Address.hh"
#include "c4Database.h"
#include "c4Replicator.h"
#include "Error.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"

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
