//
// c4Base.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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
#ifndef __cplusplus
#    error "This is C++ only"
#endif
#if !defined(LITECORE_CPP_API)
#    define LITECORE_CPP_API 1
#endif

#include "c4Base.h"
#include "RefCounted.hh"
#include "InstanceCounted.hh"
#include "fleece/slice.hh"

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


// Just a mix-in that allows API class declarations to use common Fleece types un-namespaced:
struct C4Base {
    using slice = fleece::slice;
    using alloc_slice = fleece::alloc_slice;
    template <class T> using Retained = fleece::Retained<T>;
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
    }

    namespace REST {
        class Listener;
        class RESTListener;
    }

    namespace websocket {
        class WebSocket;
    }
}


C4_ASSUME_NONNULL_END
