//
// Address.hh
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
