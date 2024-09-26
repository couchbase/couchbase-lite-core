//
// REST_CAPI.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Listener.h"
#include "c4Listener.hh"
#include "c4ExceptionUtils.hh"
#include "fleece/Mutable.hh"

using namespace std;
using namespace fleece;
using namespace litecore;

CBL_CORE_API C4ListenerAPIs c4listener_availableAPIs(void) noexcept { return C4Listener::availableAPIs(); }

CBL_CORE_API C4Listener* c4listener_start(const C4ListenerConfig* config, C4Error* outError) noexcept {
    try {
        return new C4Listener(*config);
    }
    catchError(outError) return nullptr;
}

CBL_CORE_API void c4listener_free(C4Listener* listener) noexcept { delete listener; }

CBL_CORE_API C4StringResult c4db_URINameFromPath(C4String pathSlice) noexcept {
    try {
        if ( string name = C4Listener::URLNameFromPath(pathSlice); name.empty() ) return {};
        else
            return FLSliceResult(alloc_slice(name));
    }
    catchAndWarn() return {};
}

CBL_CORE_API bool c4listener_shareDB(C4Listener* listener, C4String name, C4Database* db, C4Error* outError) noexcept {
    try {
        return listener->shareDB(name, db);
    }
    catchError(outError)
    return false;
}

CBL_CORE_API bool c4listener_unshareDB(C4Listener* listener, C4Database* db, C4Error* outError) noexcept {
    try {
        if ( listener->unshareDB(db) ) return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotOpen, "Database not shared"_sl, outError);
    }
    catchError(outError)
    return false;
}

CBL_CORE_API bool c4listener_shareCollection(C4Listener* listener, C4String name, C4Collection* collection,
                                             C4Error* outError) noexcept {
    try {
        return listener->shareCollection(name, collection);
    }
    catchError(outError)
    return false;
}

CBL_CORE_API bool c4listener_unshareCollection(C4Listener* listener, C4String name, C4Collection* collection,
                                               C4Error* outError) noexcept {
    try {
        if ( listener->unshareCollection(name, collection) ) return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotOpen, "Collection not shared"_sl, outError);
    }
    catchError(outError)
    return false;
}

CBL_CORE_API uint16_t c4listener_getPort(const C4Listener* listener) noexcept {
    try {
        return listener->port();
    }
    catchAndWarn() return 0;
}

CBL_CORE_API FLMutableArray c4listener_getURLs(const C4Listener* listener, C4Database* db, C4ListenerAPIs api,
                                               C4Error* err) noexcept {
    try {
        auto urls = fleece::MutableArray::newArray();
        for ( string& url : listener->URLs(db, api) ) urls.append(url);
        return (FLMutableArray)FLValue_Retain(urls);
    }
    catchError(err)
    return nullptr;
}

CBL_CORE_API void c4listener_getConnectionStatus(const C4Listener* listener, unsigned* connectionCount,
                                                 unsigned* activeConnectionCount) noexcept {
    auto [conns, active] = listener->connectionStatus();
    if ( connectionCount ) *connectionCount = conns;
    if ( activeConnectionCount ) *activeConnectionCount = active;
}
