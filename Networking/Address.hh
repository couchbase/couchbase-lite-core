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
#include "fleece/PlatformCompat.hh"
#include <fleece/slice.hh>

struct C4Database;

namespace litecore { namespace net {

    /** Enhanced C4Address subclass that allocates storage for the fields. */
    struct Address : public C4Address {
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        explicit Address(const alloc_slice &url);
        explicit Address(slice url)                         :Address(alloc_slice(url)) { }
        explicit Address(const C4Address&);
        explicit Address(C4Database* NONNULL);

        Address(slice scheme,
                slice hostname,
                uint16_t port,
                slice uri);

        Address& operator= (const Address &addr);

        alloc_slice url() const                             {return _url;}
        operator alloc_slice () const                       {return _url;}

        bool isSecure() const noexcept                      {return isSecure(*(C4Address*)this);}

        // Static utility functions:
        static alloc_slice toURL(const C4Address&) noexcept;
        static bool isSecure(const C4Address&) noexcept;
        static bool domainEquals(slice d1, slice d2) noexcept;
        static bool domainContains(slice baseDomain, slice hostname) noexcept;
        static bool pathContains(slice basePath, slice path) noexcept;

    private:
        alloc_slice _url;         // inherited slice fields point inside this
    };

} }
