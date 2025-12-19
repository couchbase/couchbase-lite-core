//
// TLSCodec.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.h"

C4_ASSUME_NONNULL_BEGIN

namespace litecore::net {
    class TLSContext;

    /** Wraps a C4SocketFactory to add TLS to it.
     *  @param factory  The underlying stream-based socket factory.
     *  @param nativeHandle  A `nativeHandle` for `factory`, if its connection is already open.
     *  @param tlsContext  A configured TLSContext.
     *  @returns  A new C4SocketFactory and nativeHandle to open a C4Socket with. */
    std::pair<C4SocketFactory,void*> wrapSocketInTLS(const C4SocketFactory& factory,
                                                     void* C4NULLABLE nativeHandle,
                                                     TLSContext* tlsContext);
}

C4_ASSUME_NONNULL_END
