//
// Address.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4ReplicatorTypes.h"
#include <fleece/slice.hh>

struct C4Database;

namespace litecore::net {

    /** Enhanced C4Address subclass that allocates storage for the fields. */
    struct Address {
        using slice       = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        explicit Address(alloc_slice url);

        explicit Address(slice url) : Address(alloc_slice(url)) {}

        explicit Address(const C4Address&);
        explicit Address(C4Database* NONNULL);

        Address(slice scheme, slice hostname, uint16_t port, slice uri);

        Address& operator=(const Address& addr);

        [[nodiscard]] alloc_slice url() const { return _url; }

        explicit operator alloc_slice() const { return _url; }

        operator C4Address() const { return _c4address; }  // NOLINT(google-explicit-constructor)

        explicit operator C4Address*() { return &_c4address; }

        [[nodiscard]] C4String scheme() const { return _c4address.scheme; }

        [[nodiscard]] C4String hostname() const { return _c4address.hostname; }

        [[nodiscard]] uint16_t port() const { return _c4address.port; }

        [[nodiscard]] C4String path() const { return _c4address.path; }

        [[nodiscard]] bool isSecure() const noexcept { return isSecure(_c4address); }

        // Static utility functions:
        static alloc_slice toURL(const C4Address&) noexcept;
        static bool        isSecure(const C4Address&) noexcept;
        static bool        domainEquals(slice d1, slice d2) noexcept;
        static bool        domainContains(slice baseDomain, slice hostname) noexcept;
        static bool        pathContains(slice basePath, slice path) noexcept;

      private:
        alloc_slice _url;  // inherited slice fields point inside this
        C4Address   _c4address;
    };

}  // namespace litecore::net
