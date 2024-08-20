//
// c4Base.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#ifndef __cplusplus
#    error "This is C++ only"
#endif
#if !defined(LITECORE_CPP_API)
#    define LITECORE_CPP_API 1
#endif

#include "c4Compat.h"
#include "fleece/RefCounted.hh"
#include "fleece/slice.hh"

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


// Just a mix-in that allows API class declarations to use common Fleece types un-namespaced:
struct C4Base {
    using slice       = fleece::slice;
    using alloc_slice = fleece::alloc_slice;
    template <class T>
    using Retained = fleece::Retained<T>;
};

// Forward references to internal LiteCore classes named in the public headers
namespace litecore {
    class BlobStore;
    class BlobWriteStream;
    class C4CollectionObserverImpl;
    class C4DocumentObserverImpl;
    class C4QueryEnumeratorImpl;
    class C4QueryObserverImpl;
    class CollectionImpl;
    class DatabaseImpl;
    class ExclusiveTransaction;
    class FilePath;
    class KeyStore;
    class LazyIndexUpdate;
    class LiveQuerier;
    class Query;
    class QueryEnumerator;
    class Record;
    class revid;
    class SeekableReadStream;
    class SequenceTracker;
    class Upgrader;

    namespace crypto {
        class Cert;
        class CertBase;
        class CertSigningRequest;
        class Key;
        class PersistentPrivateKey;
        class PrivateKey;
        class PublicKey;
    }  // namespace crypto

    namespace REST {
        class Listener;
        class HTTPListener;
    }  // namespace REST

    namespace websocket {
        class WebSocket;
    }
}  // namespace litecore

C4_ASSUME_NONNULL_END
