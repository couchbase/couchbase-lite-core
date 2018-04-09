//
//  Address.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/6/18.
//Copyright © 2018 Couchbase. All rights reserved.
//

#include "FleeceCpp.hh"
#include "Address.hh"
#include "c4Replicator.h"
#include "Error.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;


namespace litecore { namespace repl {

    Address::Address(const alloc_slice &url)
    :_url(url)
    {
        if (!c4address_fromURL(_url, this, nullptr))
            error::_throw(error::Network, kC4NetErrInvalidURL);
    }


    Address::Address(const C4Address &addr)
    :Address(c4address_toURL(addr))
    { }


    static alloc_slice dbURL(C4Database *db) {
        alloc_slice path(c4db_getPath(db));
        return alloc_slice(string("file://") + string(path));
    }


    Address::Address(C4Database *db)
    :Address( dbURL(db) )
    { }


    alloc_slice Address::toURL(const C4Address &c4Addr) noexcept {
        return c4address_toURL(c4Addr);
    }


    bool Address::isSecure(const C4Address &addr) noexcept {
        return (addr.scheme == "wss"_sl || addr.scheme == "https"_sl
                || addr.scheme == "blips"_sl);
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
            return true;
        if (path.size == 0)
            return false;
        return path.hasPrefix(basePath)
            && (path.size == basePath.size
                || path[basePath.size] == '/'
                || basePath[basePath.size-1] == '/');
    }

} }
