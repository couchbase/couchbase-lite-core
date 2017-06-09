//
//  Address.cc
//  blip_cpp
//
//  Created by Jens Alfke on 6/9/17.
//Copyright © 2017 Couchbase. All rights reserved.
//

#include "Address.hh"
#include "Error.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;

namespace litecore { namespace websocket {

    Address::Address(const string &scheme_, const string &hostname_,
                     uint16_t port_, const string &path_)
    :scheme(scheme_)
    ,hostname(hostname_)
    ,port(port_ ? port_ : defaultPort())
    ,path(path_)
    { }


    bool Address::isSecure() const {
        return (scheme == "wss" || scheme == "https" || scheme == "blips");
    }

    uint16_t Address::defaultPort() const {
        return isSecure() ? 443 : 80;
    }

    Address::operator string() const {
        stringstream result;
        result << scheme << ':' << hostname;
        if (port != defaultPort())
            result << ':' << port;
        if (path.empty() || path[0] != '/')
            result << '/';
        result << path;
        return result.str();
    }


    bool Address::domainEquals(const std::string &d1, const std::string &d2) {
        return compareIgnoringCase(d1, d2);
    }

    bool Address::domainContains(const string &baseDomain, const string &hostname) {
        return hasSuffixIgnoringCase(hostname, baseDomain)
        && (hostname.size() == baseDomain.size()
            || hostname[hostname.size() - baseDomain.size() - 1] == '.');
    }

    bool Address::pathContains(const string &basePath, const string &path) {
        if (basePath.empty())
            return true;
        if (path.empty())
            return false;
        return hasPrefix(path, basePath)
        && (path.size() == basePath.size()
            || path[basePath.size()] == '/'
            || basePath[basePath.size()-1] == '/');
    }


} }
