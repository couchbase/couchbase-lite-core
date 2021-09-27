//
// NetworkInterfaces.hh
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sockaddr;
struct in_addr;
struct in6_addr;

namespace sockpp {
    class sock_address;
}

namespace litecore::net {

    /// Represents an IP address of a network interface.
    class IPAddress {
    public:
        IPAddress(const sockaddr&) noexcept;
        IPAddress(const in_addr&) noexcept;
        IPAddress(const in6_addr&) noexcept;

        static std::optional<IPAddress> parse(const std::string&);

        int family() const                  {return _family;}  ///< AF_INET or AF_INET6
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

        std::unique_ptr<sockpp::sock_address> sockppAddress(uint16_t port) const;
        bool operator== (const IPAddress&) const;

    private:
        IPAddress() =default;
        in_addr&  _addr4();
        in6_addr& _addr6();

        std::array<int64_t,2> _data;
        uint8_t               _family;
    };


    /// Represents a network interface.
    struct Interface {
    public:
        /// Returns all active network interfaces, in descending order of priority.
        static std::vector<Interface> all();

        /// Returns the Interface with the given address, if any.
        static std::optional<Interface> withAddress(const IPAddress&);

        /// Returns each address of each active network interface.
        static std::vector<IPAddress> allAddresses(IPAddress::Scope scope =IPAddress::kLinkLocal);

        /// Returns the primary IP address of each active network interface.
        static std::vector<IPAddress> primaryAddresses();

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
    std::optional<std::string> GetMyHostName();

}
