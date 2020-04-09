//
// NetworkInterfaces.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/slice.hh"
#include <array>
#include <string>
#include <vector>

struct sockaddr;
struct in_addr;
struct in6_addr;

namespace litecore::net {

    /// Represents an IP address of a network interface.
    class IPAddress {
    public:
        IPAddress(const sockaddr &data) noexcept;

        int family() const;                 ///< AF_INET or AF_INET6
        bool isIPv4() const;
        bool isIPv6() const;
        bool isLoopback() const;
        bool isLinkLocal() const;
        bool isRoutable() const             {return scope() == kRoutable;}

        enum Scope {
            kLoopback, kLinkLocal, kRoutable
        };
        Scope scope() const;

        const in_addr&  addr4() const;
        const in6_addr& addr6() const;
        operator const in_addr& () const    {return addr4();}
        operator const in6_addr& () const   {return addr6();}

        operator std::string() const;

    private:
        union {
            std::array<uint8_t,256> _data;
            int x; //HACK: forces proper alignment
        };
    };


    /// Represents a network interface.
    struct Interface {
    public:
        /// Returns all active network interfaces, in descending order of priority.
        static std::vector<Interface> all();
        /// Returns the primary IP address of each active network interface.
        static std::vector<IPAddress> allAddresses();

        std::string             name;
        int                     flags = 0;  ///< IFF_UP, etc; see <net/if.h>
        uint8_t                 type = 0;   ///< IFT_ETHER, etc; see <net/if.h>
        std::vector<IPAddress>  addresses;  ///< Addresses in descending order of priority

        bool isLoopback() const;
        bool isRoutable() const;

        const IPAddress& primaryAddress() const;

        void dump();
    };


    /// Returns the computer's DNS or mDNS hostname if known, otherwise its primary IP address.
    std::string GetMyHostName();

}
