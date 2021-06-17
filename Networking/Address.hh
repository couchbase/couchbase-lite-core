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
#include "c4ReplicatorTypes.h"
#include "PlatformCompat.hh"
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
