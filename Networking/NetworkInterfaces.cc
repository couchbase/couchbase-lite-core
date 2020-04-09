//
// NetworkInterfaces.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "NetworkInterfaces.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "fleece/slice.hh"
#include <unordered_map>
#include <array>
#include <vector>

#include "sockpp/platform.h"        // Includes the system headers for sockaddr, etc.

#ifdef _WIN32
    //TODO: Windows includes go here
#else
    #include <ifaddrs.h>
    #include <net/if.h>
#endif

#ifdef __APPLE__
    #include <CoreFoundation/CFString.h>
    #include <SystemConfiguration/SystemConfiguration.h>
#endif

namespace litecore::net {
    using namespace std;
    using namespace fleece;


#pragma mark - IPADDRESS:


    IPAddress::IPAddress(const sockaddr &data) noexcept   {memcpy(&_data, &data, data.sa_len);}

    int IPAddress::family() const               {return ((const sockaddr*)&_data)->sa_family;}
    bool IPAddress::isIPv4() const              {return family() == AF_INET;}
    bool IPAddress::isIPv6() const              {return family() == AF_INET6;}
    const in_addr& IPAddress::addr4() const     {return ((const sockaddr_in*)&_data)->sin_addr;};
    const in6_addr& IPAddress::addr6() const    {return ((const sockaddr_in6*)&_data)->sin6_addr;};

    bool IPAddress::isLoopback() const {
        if (isIPv4())
            return ntohl(addr4().s_addr) == INADDR_LOOPBACK;
        else
            return memcmp(&addr6(), &in6addr_loopback, sizeof(in6_addr)) == 0;
    }

    bool IPAddress::isLinkLocal() const {
        if (isIPv4())
            return (ntohl(addr4().s_addr) >> 16) == 0xA9FE;  // 169.254.*.*
        else
            return (ntohs(addr6().__u6_addr.__u6_addr16[0]) & 0xFFC0) == 0xFE80; // fe80::
    }

    IPAddress::Scope IPAddress::scope() const {
        return isLoopback() ? kLoopback : (isLinkLocal() ? kLinkLocal : kRoutable);
    }

    IPAddress::operator string() const {
        char buf[INET6_ADDRSTRLEN];
        const void *addr = (isIPv4()) ? (void*)&addr4() : (void*)&addr6();
        return inet_ntop(family(), addr, buf, sizeof(buf));
    }

    static bool operator< (const IPAddress &a, const IPAddress &b) {
        return (a.family() < b.family())                            // ipv4 < ipv6
            || (a.family() == b.family() && a.scope() > b.scope()); // routable < local < loopback
    }


#pragma mark - INTERFACE:


    // Platform-specific code to read the enabled network interfaces.
    // Results are unsorted/unfiltered.
    static void _getInterfaces(vector<Interface> &interfaces);


    bool Interface::isLoopback() const     {return (flags & IFF_LOOPBACK) != 0;}
    bool Interface::isRoutable() const     {return primaryAddress().isRoutable();}

    const IPAddress& Interface::primaryAddress() const {return addresses[0];}

    static bool operator< (const Interface &a, const Interface &b) {
        return a.primaryAddress() < b.primaryAddress();
    }

    void Interface::dump() {
        fprintf(stderr, "%s [flags %04x, type %x]: ", name.c_str(), flags, type);
        for (auto &addr : addresses)
            fprintf(stderr, "%s, ", string(addr).c_str());
        fprintf(stderr, "\n");
    }

    vector<Interface> Interface::all() {
        vector<Interface> interfaces;
        _getInterfaces(interfaces);
        
        for (auto i = interfaces.begin(); i != interfaces.end();) {
            auto &intf = *i;
            if (intf.addresses.empty()) {
                interfaces.erase(i);
                continue;
            }
            sort(intf.addresses.begin(), intf.addresses.end());
            if (intf.addresses[0].isLinkLocal() && intf.addresses[0].isIPv6()) {
                // As a heuristic, ignore interfaces that have _only_ link-local IPv6 addresses,
                // since IPv6 requires that _every_ interface have a link-local address.
                // Such interfaces are likely to be inactive.
                interfaces.erase(i);
                continue;
            }
            ++i;
        }
        sort(interfaces.begin(), interfaces.end());
        return interfaces;
    }

    std::vector<IPAddress> Interface::allAddresses() {
        vector<IPAddress> addresses;
        for (auto &intf : Interface::all())
            addresses.push_back(intf.addresses[0]);
        return addresses;
    }


#pragma mark - PLATFORM SPECIFIC CODE:


    string GetMyHostName() {
#ifdef __APPLE__
        string hostName;
    #if TARGET_OS_OSX
        // Quinn says this is the correct call to use (on Mac; it's not available on iOS)
        CFStringRef cfName = SCDynamicStoreCopyLocalHostName(NULL);
        if (cfName) {
            nsstring_slice strsl(cfName);
            hostName = string(strsl);
        }
    #else
        // From <http://stackoverflow.com/a/16902907/98077>
        char baseHostName[256];
        if (gethostname(baseHostName, 255) == 0) {
            baseHostName[255] = '\0';
            hostName = baseHostName;
        }
    #endif
        // On Apple platforms the hostname is the mDNS/Bonjour hostname:
        if (!hostName.empty()) {
            if (!hasSuffix(hostName, ".local"))
                hostName += ".local";
            return hostName;
        }
#endif
        // Fallback: just return the (numeric) primary IP address.
        auto addresses = Interface::allAddresses();
        return addresses.empty() ? "" : string(addresses[0]);
    }


    // Platform-specific code to read the enabled network interfaces.
    // Results are unsorted/unfiltered.
    static void _getInterfaces(vector<Interface> &interfaces) {
#ifdef _WIN32
        //TODO: Implement _getInterfaces on Windows
        error::_throw(error::Unimplemented, "net::Interface not implemented yet on Windows");
#else
        struct ifaddrs *addrs;
        if (getifaddrs(&addrs) < 0)
            error::_throwErrno();
        const char *lastName = nullptr;
        Interface *intf = nullptr;
        for (auto a = addrs; a; a = a->ifa_next) {
            if (a->ifa_flags & IFF_UP) {
                if (!lastName || strcmp(lastName, a->ifa_name) != 0) {
                    lastName = a->ifa_name;
                    intf = &interfaces.emplace_back();
                    intf->name = a->ifa_name;
                    intf->flags = a->ifa_flags;
                }
                switch (a->ifa_addr->sa_family) {
                    case AF_LINK:
                        intf->type = ((const if_data *)a->ifa_data)->ifi_type;
                        break;
                    case AF_INET:
                    case AF_INET6:
                        intf->addresses.push_back(*a->ifa_addr);
                        break;
                }
            }
        }
        freeifaddrs(addrs);
#endif
    }

}
