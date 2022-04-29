//
// NetworkInterfaces.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "NetworkInterfaces.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "fleece/slice.hh"
#include <unordered_map>
#include <array>
#include <vector>
#include <map>

#include "sockpp/platform.h"        // Includes the system headers for sockaddr, etc.
#include "sockpp/inet_address.h"
#include "sockpp/inet6_address.h"

#ifdef _WIN32
    #include <IPHlpApi.h>
    #include <atlconv.h>

    #pragma comment(lib, "iphlpapi")
#else
    #include <ifaddrs.h>
    #include <net/if.h>
#endif

#ifdef __APPLE__
    #include <CoreFoundation/CFString.h>
    #include <SystemConfiguration/SystemConfiguration.h>
#endif

#ifdef __ANDROID__
    #include "getifaddrs.h"
#endif

namespace litecore::net {
    using namespace std;
    using namespace fleece;


#pragma mark - IPADDRESS:


    IPAddress::IPAddress(const sockaddr &addr) noexcept
    :_family(addr.sa_family)
    {
        static_assert(sizeof(_data) >= sizeof(in_addr));
        static_assert(sizeof(_data) >= sizeof(in6_addr));
        Assert(_family == AF_INET || _family == AF_INET6);
        if (_family == AF_INET)
            _addr4() = ((const sockaddr_in&)addr).sin_addr;
        else
            _addr6() = ((const sockaddr_in6&)addr).sin6_addr;
            
    }

    IPAddress::IPAddress(const in_addr &addr) noexcept
    :_family(AF_INET)
    {
        _addr4() = addr;
    }

    IPAddress::IPAddress(const in6_addr &addr) noexcept
    :_family(AF_INET6)
    {
        _addr6() = addr;
    }

    optional<IPAddress> IPAddress::parse(const string &str) {
        IPAddress addr;
        if (::inet_pton(AF_INET, str.c_str(), &addr._data) == 1) {
            addr._family = AF_INET;
        } else if (::inet_pton(AF_INET6, str.c_str(), &addr._data) == 1) {
            addr._family = AF_INET6;
        } else {
            return nullopt;
        }
        return addr;
    }

    bool IPAddress::isIPv4() const              {return _family == AF_INET;}
    bool IPAddress::isIPv6() const              {return _family == AF_INET6;}
    in_addr& IPAddress::_addr4()                {return *(in_addr*)&_data;}
    in6_addr& IPAddress::_addr6()               {return *(in6_addr*)&_data;};
    const in_addr& IPAddress::addr4() const     {return *(const in_addr*)&_data;}
    const in6_addr& IPAddress::addr6() const    {return *(const in6_addr*)&_data;};

    bool IPAddress::isLoopback() const {
        if (isIPv4())
            return ntohl(addr4().s_addr) == INADDR_LOOPBACK;
        else
            return memcmp(&addr6(), &in6addr_loopback, sizeof(in6_addr)) == 0;
    }

    bool IPAddress::isLinkLocal() const {
        if (isIPv4())
            return (ntohl(addr4().s_addr) >> 16) == 0xA9FE;  // 169.254.*.*
        else {
#ifdef WIN32
            const auto firstBytePair = addr6().u.Word[0];
#elif defined(__APPLE__)
            const auto firstBytePair = addr6().__u6_addr.__u6_addr16[0];
#elif defined(__ANDROID__)
            const auto firstBytePair = addr6().in6_u.u6_addr16[0];
#else
            const auto firstBytePair = addr6().__in6_u.__u6_addr16[0];
#endif
            return (ntohs(firstBytePair) & 0xFFC0) == 0xFE80; // fe80::
        }
    }

    IPAddress::Scope IPAddress::scope() const {
        return isLoopback() ? kLoopback : (isLinkLocal() ? kLinkLocal : kRoutable);
    }

    IPAddress::operator string() const {
        char buf[INET6_ADDRSTRLEN];
        const void *addr = (isIPv4()) ? (void*)&addr4() : (void*)&addr6();
        return inet_ntop(_family, addr, buf, sizeof(buf));
    }

    unique_ptr<sockpp::sock_address> IPAddress::sockppAddress(uint16_t port) const {
        if (isIPv4()) {
            return make_unique<sockpp::inet_address>(ntohl(addr4().s_addr), port);
        } else {
            auto addr = make_unique<sockpp::inet6_address>();
            addr->create(addr6(), port);
            return addr;
        }
    }

    bool IPAddress::operator== (const IPAddress &b) const {
        if (_family != b._family)
            return false;
        else if (isIPv4())
            return addr4().s_addr == b.addr4().s_addr;
        else
            return memcmp(&addr6(), &b.addr6(), sizeof(in6_addr)) == 0;
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
                i = interfaces.erase(i);
                continue;
            }
            sort(intf.addresses.begin(), intf.addresses.end());
            if (intf.addresses[0].isLinkLocal() && intf.addresses[0].isIPv6()) {
                // As a heuristic, ignore interfaces that have _only_ link-local IPv6 addresses,
                // since IPv6 requires that _every_ interface have a link-local address.
                // Such interfaces are likely to be inactive.
                i = interfaces.erase(i);
                continue;
            }
            ++i;
        }
        sort(interfaces.begin(), interfaces.end());
        return interfaces;
    }

    optional<Interface> Interface::withAddress(const IPAddress &addr) {
        for (auto &intf : Interface::all()) {
            for (auto &ifAddr : intf.addresses) {
                if (ifAddr == addr)
                    return intf;
            }
        }
        return nullopt;
    }

    std::vector<IPAddress> Interface::allAddresses(IPAddress::Scope scope) {
        vector<IPAddress> allAddrs;
        for (auto &intf : Interface::all()) {
            for (auto &addr : intf.addresses) {
                if (addr.scope() >= scope)
                    allAddrs.push_back(addr);
            }
        }
        return allAddrs;
    }

    std::vector<IPAddress> Interface::primaryAddresses() {
        vector<IPAddress> addresses;
        for (auto &intf : Interface::all())
            addresses.push_back(intf.addresses[0]);
        return addresses;
    }


#pragma mark - PLATFORM SPECIFIC CODE:


    optional<string> GetMyHostName() {
#ifdef __APPLE__
        // Apple platforms always have an mDNS/Bonjour hostname.
        string hostName;
    #if TARGET_OS_OSX
        // On macOS, we can get it from SystemConfiguration (not available on iOS)
        if (CFStringRef cfName = SCDynamicStoreCopyLocalHostName(NULL); cfName) {
            nsstring_slice strsl(cfName);
            hostName = string(strsl);
        }
    #else
        // From <http://stackoverflow.com/a/16902907/98077>.
        // On iOS, gethostname() returns the mDNS/Bonjour hostname (without the ".local")
        char baseHostName[256];
        if (gethostname(baseHostName, 255) == 0) {
            baseHostName[255] = '\0';
            hostName = baseHostName;
        }
    #endif
        if (!hostName.empty()) {
            if (!hasSuffix(hostName, ".local"))
                hostName += ".local";
            return hostName;
        }
#endif
        //TODO: Android supports mDNS; is there an API to get the hostname?
        return nullopt;
    }


    // Platform-specific code to read the enabled network interfaces.
    // Results are unsorted/unfiltered.
    static void _getInterfaces(vector<Interface> &interfaces) {
#ifdef _WIN32
        auto info = static_cast<IP_ADAPTER_ADDRESSES*>(HeapAlloc(GetProcessHeap(), 0, sizeof(IP_ADAPTER_ADDRESSES)));
        ULONG bufferSize = sizeof(IP_ADAPTER_ADDRESSES);
        const DWORD flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        auto result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, info, &bufferSize);
        if (result == ERROR_BUFFER_OVERFLOW) {
            HeapFree(GetProcessHeap(), 0, info);
            info = static_cast<IP_ADAPTER_ADDRESSES*>(HeapAlloc(GetProcessHeap(), 0, bufferSize));
            result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, info, &bufferSize);
        }

        if (result == ERROR_NO_DATA) {
            return;
        }

        if (result == ERROR_OUTOFMEMORY) {
            throw std::bad_alloc();
        }

        if (result != ERROR_SUCCESS) {
            // If any of these are between 100 and 140 they will be reported
            // incorrectly
            error(error::Domain::POSIX, result)._throw();
        }

        PIP_ADAPTER_ADDRESSES current = info;
        while (current) {
            if (current->OperStatus == IfOperStatusUp &&
                (current->IfType == IF_TYPE_ETHERNET_CSMACD ||
                 current->IfType == IF_TYPE_IEEE80211 ||
                 current->IfType == IF_TYPE_SOFTWARE_LOOPBACK)) {
                auto intf = &interfaces.emplace_back();
                const ATL::CW2AEX<256> convertedPath(current->FriendlyName, CP_UTF8);
                intf->name = convertedPath.m_psz;
                intf->type = current->IfType;
                intf->flags = 0;
                auto address = current->FirstUnicastAddress;
                while (address) {
                    intf->addresses.emplace_back(*address->Address.lpSockaddr);
                    address = address->Next;
                }
            }

            current = current->Next;
        }

        HeapFree(GetProcessHeap(), 0, info);
#else
        // These interfaces don't show up in consistent order.  On macOS they show up as
        // interface1 / addr1, interface1 / addr2, interface1 / addrX, interface2 / addr1, ...
        // but on Linux (at least Ubuntu) they show up as
        // interface 1 / addr1, interface 2 / addr1, interfaceX / addr1, interface1 / addr2, ...
        // so this map will keep track of the ones we've seen so far so we can add on the
        // addresses to them instead of erroneously creating duplicate interfaces
        map<string, size_t> results;
        struct ifaddrs *addrs;
        if (getifaddrs(&addrs) < 0)
            error::_throwErrno();
        
        Interface *intf = nullptr;
        for (auto a = addrs; a; a = a->ifa_next) {
            auto nextIterator = results.find(a->ifa_name);
            if(nextIterator == results.end()) {
                results.emplace(a->ifa_name, interfaces.size());
                intf = &interfaces.emplace_back();
            } else {
                intf = &interfaces[nextIterator->second];
            }
            
            if ((a->ifa_flags & IFF_UP) != 0 && a->ifa_addr != nullptr) {
                intf->name = a->ifa_name;
                intf->flags = a->ifa_flags;
                switch (a->ifa_addr->sa_family) {
#ifdef __APPLE__
                    case AF_LINK:
                        intf->type = ((const if_data *)a->ifa_data)->ifi_type;
                        break;
#endif
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
