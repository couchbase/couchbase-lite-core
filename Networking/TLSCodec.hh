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

    /** Wraps a C4SocketFactory to add TLS to it, before attaching to an outgoing C4Socket.
     *  @param factory  The underlying stream-based socket factory.
     *  @returns  A new C4SocketFactory to open a C4Socket with. */
    C4SocketFactory wrapSocketFactoryInTLS(const C4SocketFactory& factory);

    /** Wraps a C4SocketFactory to add TLS to it, for creating an incoming C4Socket.
     *  Used by \ref C4Socket::fromNative.
     *  @param factory  The underlying stream-based socket factory.
     *  @param nativeHandle  The `nativeHandle` associated with `factory`.
     *  @param tlsContext  A configured TLSContext.
     *  @returns  A new C4SocketFactory and nativeHandle to bind to a C4Socket. */
    std::pair<C4SocketFactory, void*> wrapSocketFactoryInTLS(const C4SocketFactory& factory, void* nativeHandle,
                                                             TLSContext* tlsContext);
}  // namespace litecore::net

C4_ASSUME_NONNULL_END
