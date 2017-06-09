//
//  Address.hh
//  blip_cpp
//
//  Created by Jens Alfke on 6/9/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include <sstream>
#include <string>

namespace litecore { namespace websocket {

    /** Basically a low-budget URL class. */
    struct Address {
        const std::string scheme;
        const std::string hostname;
        const uint16_t port;
        const std::string path;             // technically this is the "resource specifier"

        Address(const std::string &scheme_, const std::string &hostname_,
                uint16_t port_ =0, const std::string &path_ ="/");

        Address(const std::string &hostname_,
                uint16_t port_ =0, const std::string &path_ ="/")
        :Address("ws", hostname_, port_, path_)
        { }

        bool isSecure() const;

        uint16_t defaultPort() const;

        operator std::string() const;

        // Static utility functions for comparing hostnames/domains and paths:
        static bool domainEquals(const std::string &d1, const std::string &d2);
        static bool domainContains(const std::string &baseDomain, const std::string &hostname);
        static bool pathContains(const std::string &basePath, const std::string &path);

    };

} }
