//
// c4Replicator_CAPI.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Replicator.hh"
#include "c4Database.hh"
#include "c4Socket.hh"
#include "c4Socket+Internal.hh"
#include "c4ExceptionUtils.hh"
#include "c4Private.h"
#include "c4Replicator.h"
#include "c4Socket.h"
#include "Address.hh"
#include "Headers.hh"
#include "Logging.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;
using namespace litecore;


#pragma mark - REPLICATOR:

bool c4repl_isValidDatabaseName(C4String dbName) noexcept { return C4Replicator::isValidDatabaseName(dbName); }

bool c4repl_isValidRemote(C4Address addr, C4String dbName, C4Error* outError) noexcept {
    return addr.isValidRemote(dbName, outError);
}

bool c4address_fromURL(C4String url, C4Address* address, C4String* dbName) noexcept {
    return C4Address::fromURL(url, address, (slice*)dbName);
}

C4StringResult c4address_toURL(C4Address address) noexcept {
    try {
        return C4StringResult(address.toURL());
    }
    catchError(nullptr);
    return {};
}

C4Replicator* c4repl_new(C4Database* db, C4Address serverAddress, C4String remoteDatabaseName,
                         C4ReplicatorParameters params, C4Error* outError) noexcept {
    try {
        return db->newReplicator(serverAddress, remoteDatabaseName, params).detach();
    }
    catchError(outError);
    return nullptr;
}


#ifdef COUCHBASE_ENTERPRISE
C4Replicator* c4repl_newLocal(C4Database* db, C4Database* otherLocalDB, C4ReplicatorParameters params,
                              C4Error* outError) noexcept {
    try {
        return db->newLocalReplicator(otherLocalDB, params).detach();
    }
    catchError(outError);
    return nullptr;
}
#endif


C4Replicator* c4repl_newWithSocket(C4Database* db, C4Socket* openSocket, C4ReplicatorParameters params,
                                   C4Error* outError) noexcept {
    try {
        return db->newIncomingReplicator(openSocket, params).detach();
    }
    catchError(outError);
    return nullptr;
}

void c4repl_start(C4Replicator* repl, bool reset) noexcept { repl->start(reset); }

void c4repl_stop(C4Replicator* repl) noexcept { repl->stop(); }

bool c4repl_retry(C4Replicator* repl, C4Error* outError) noexcept {
    return tryCatch<bool>(nullptr, [&] {
        if ( repl->retry() ) return true;
        clearError(outError);
        return false;
    });
}

void c4repl_setHostReachable(C4Replicator* repl, bool reachable) noexcept { repl->setHostReachable(reachable); }

void c4repl_setSuspended(C4Replicator* repl, bool suspended) noexcept { repl->setSuspended(suspended); }

void c4repl_setOptions(C4Replicator* repl, C4Slice optionsDictFleece) noexcept { repl->setOptions(optionsDictFleece); }

void c4repl_free(C4Replicator* repl) noexcept {
    if ( !repl ) return;
    repl->stopCallbacks();
    release(repl);
}

C4ReplicatorStatus c4repl_getStatus(C4Replicator* repl) noexcept { return repl->getStatus(); }

C4Slice c4repl_getResponseHeaders(C4Replicator* repl) noexcept { return repl->getResponseHeaders(); }

C4SliceResult c4repl_getPendingDocIDs(C4Replicator* repl, C4CollectionSpec spec, C4Error* outErr) noexcept {
    try {
        *outErr = {};
        return C4SliceResult(repl->pendingDocIDs(spec));
    }
    catchError(outErr);
    return {};
}

bool c4repl_isDocumentPending(C4Replicator* repl, C4Slice docID, C4CollectionSpec spec, C4Error* outErr) noexcept {
    try {
        *outErr = {};
        return repl->isDocumentPending(docID, spec);
    }
    catchError(outErr);
    return false;
}

C4Cert* c4repl_getPeerTLSCertificate(C4Replicator* repl, C4Error* outErr) noexcept {
#ifdef COUCHBASE_ENTERPRISE
    *outErr = {};
    return repl->getPeerTLSCertificate();
#else
    outErr->domain = LiteCoreDomain;
    outErr->code   = kC4ErrorUnsupported;
    return nullptr;
#endif
}

bool c4repl_setProgressLevel(C4Replicator* repl, C4ReplicatorProgressLevel level, C4Error* outErr) noexcept {
    if ( _usuallyFalse(repl == nullptr) ) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("repl was null"), outErr);
        return false;
    }

    if ( _usuallyFalse(level < kC4ReplProgressOverall || level > kC4ReplProgressPerAttachment) ) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("level out of range"), outErr);
        return false;
    }

    repl->setProgressLevel(level);
    return true;
}

#pragma mark - SOCKET:


#define catchForSocket(S)                                                                                              \
    catch ( const std::exception& x ) {                                                                                \
        internal(S)->closeWithException(x);                                                                            \
    }

void c4socket_registerFactory(C4SocketFactory factory) noexcept {
    C4Socket::registerFactory(factory);
    // the only exception this can throw is a fatal logic error, so no need to catch it
}

C4Socket* c4socket_fromNative(C4SocketFactory factory, void* nativeHandle, const C4Address* address) noexcept {
    return tryCatch<C4Socket*>(nullptr, [&] {
        auto ret = C4Socket::fromNative(factory, nativeHandle, *address);
        c4SocketTrace::traces().addEvent(ret, "c4socket_fromNative");
        return ret;
    });
}

void c4Socket_setNativeHandle(C4Socket* socket, void* handle) noexcept { socket->setNativeHandle(handle); }

void* C4NULLABLE c4Socket_getNativeHandle(C4Socket* socket) noexcept { return socket->getNativeHandle(); }

inline repl::C4SocketImpl* internal(C4Socket* s) { return (repl::C4SocketImpl*)s; }

C4Socket* C4NULLABLE c4socket_retain(C4Socket* C4NULLABLE socket) C4API {
    c4SocketTrace::traces().addEvent(socket, "c4socket_retain");
    retain(internal(socket));
    return socket;
}

void c4socket_release(C4Socket* C4NULLABLE socket) C4API {
    c4SocketTrace::traces().addEvent(socket, "c4socket_release");
    release(internal(socket));
}

void c4socket_gotHTTPResponse(C4Socket* socket, int status, C4Slice responseHeadersFleece) noexcept {
    socket->gotHTTPResponse(status, responseHeadersFleece);
}

void c4socket_opened(C4Socket* socket) noexcept {
    c4SocketTrace::traces().addEvent(socket, "socket_opened");
    socket->opened();
}

void c4socket_closeRequested(C4Socket* socket, int status, C4String message) noexcept {
    c4SocketTrace::traces().addEvent(socket, "socket_closeRequested");
    socket->closeRequested(status, message);
}

void c4socket_closed(C4Socket* socket, C4Error error) noexcept {
    c4SocketTrace::traces().addEvent(socket, "socket_closed", error.code == 0 ? "normal" : "error");
    socket->closed(error);
}

void c4socket_completedWrite(C4Socket* socket, size_t byteCount) noexcept { socket->completedWrite(byteCount); }

void c4socket_received(C4Socket* socket, C4Slice data) noexcept { socket->received(data); }
