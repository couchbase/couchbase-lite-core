//
// c4Replicator_CAPI.cc
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

#include "c4Replicator.h"
#include "c4Replicator.hh"
#include "c4Database.hh"
#include "c4Private.h"
#include "c4Socket+Internal.hh"
#include "c4ExceptionUtils.hh"
#include "Address.hh"
#include "Headers.hh"
#include "Logging.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;
using namespace litecore;


#pragma mark - REPLICATOR:


bool c4repl_isValidDatabaseName(C4String dbName) C4API {
    return C4Replicator::isValidDatabaseName(dbName);
}


bool c4repl_isValidRemote(C4Address addr, C4String dbName, C4Error *outError) C4API {
    return addr.isValidRemote(dbName, outError);
}


bool c4address_fromURL(C4String url, C4Address *address, C4String *dbName) C4API {
    return C4Address::fromURL(url, address, (slice*)dbName);
}


C4StringResult c4address_toURL(C4Address address) C4API {
    try {
        return C4StringResult(address.toURL());
    } catchError(nullptr);
    return {};
}


C4Replicator* c4repl_new(C4Database* db,
                         C4Address serverAddress,
                         C4String remoteDatabaseName,
                         C4ReplicatorParameters params,
                         C4Error *outError) C4API
{
    try {
        return db->newReplicator(serverAddress, remoteDatabaseName, params).detach();
    } catchError(outError);
    return nullptr;
}


#ifdef COUCHBASE_ENTERPRISE
C4Replicator* c4repl_newLocal(C4Database* db,
                              C4Database* otherLocalDB,
                              C4ReplicatorParameters params,
                              C4Error *outError) C4API
{
    try {
        return db->newLocalReplicator(otherLocalDB, params).detach();
    } catchError(outError);
    return nullptr;
}
#endif

C4Replicator* c4repl_newWithWebSocket(C4Database* db,
                                      litecore::websocket::WebSocket *openSocket,
                                      C4ReplicatorParameters params,
                                      C4Error *outError) C4API
{
    try {
        return db->newReplicator(openSocket, params).detach();
    } catchError(outError);
    return nullptr;
}


C4Replicator* c4repl_newWithSocket(C4Database* db,
                                   C4Socket *openSocket,
                                   C4ReplicatorParameters params,
                                   C4Error *outError) C4API
{
    return c4repl_newWithWebSocket(db, litecore::repl::WebSocketFrom(openSocket), params, outError);
}


void c4repl_start(C4Replicator* repl, bool reset) C4API {
    repl->start(reset);
}


void c4repl_stop(C4Replicator* repl) C4API {
    repl->stop();
}


bool c4repl_retry(C4Replicator* repl, C4Error *outError) C4API {
    return tryCatch<bool>(nullptr, [&] {
        if (repl->retry())
            return true;
        clearError(outError);
        return false;
    });
}


void c4repl_setHostReachable(C4Replicator* repl, bool reachable) C4API {
    repl->setHostReachable(reachable);
}


void c4repl_setSuspended(C4Replicator* repl, bool suspended) C4API {
    repl->setSuspended(suspended);
}


void c4repl_setOptions(C4Replicator* repl, C4Slice optionsDictFleece) C4API {
    repl->setOptions(optionsDictFleece);
}


void c4repl_free(C4Replicator* repl) C4API {
    if (!repl)
        return;
    repl->stopCallbacks();
    release(repl);
}


C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl) C4API {
    return repl->getStatus();
}


C4Slice c4repl_getResponseHeaders(C4Replicator *repl) C4API {
    return repl->getResponseHeaders();
}


C4SliceResult c4repl_getPendingDocIDs(C4Replicator* repl, C4Error* outErr) C4API {
    try {
        return C4SliceResult( repl->pendingDocIDs() );
    } catchError(outErr);
    return {};
}


bool c4repl_isDocumentPending(C4Replicator* repl, C4Slice docID, C4Error* outErr) C4API {
    try {
        return repl->isDocumentPending(docID);
    } catchError(outErr);
    return false;
}

C4Cert* c4repl_getPeerTLSCertificate(C4Replicator* repl, C4Error* outErr) C4API {
#ifdef COUCHBASE_ENTERPRISE
    outErr->code = 0;
    return repl->getPeerTLSCertificate();
#else
    outErr->domain = LiteCoreDomain;
    outErr->code = kC4ErrorUnsupported;
    return nullptr;
#endif
}


bool c4repl_setProgressLevel(C4Replicator* repl, C4ReplicatorProgressLevel level, C4Error* outErr) C4API {
    if(_usuallyFalse(repl == nullptr)) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("repl was null"), outErr);
        return false;
    }

    if(_usuallyFalse(level < kC4ReplProgressOverall || level > kC4ReplProgressPerAttachment)) {
        c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("level out of range"), outErr);
        return false;
    }

    repl->setProgressLevel(level);
    return true;
}


#pragma mark - SOCKET:


//TODO: Create a C++ API from this


using namespace litecore::net;
using namespace litecore::repl;
using namespace litecore::websocket;


static C4SocketImpl* internal(C4Socket *s)  {return (C4SocketImpl*)s;}

#define catchForSocket(S) \
    catch (const std::exception &x) {internal(S)->closeWithException(x);}


void c4socket_registerFactory(C4SocketFactory factory) C4API {
    C4SocketImpl::registerFactory(factory);
    // the only exception this can throw is a fatal logic error, so no need to catch it
}

C4Socket* c4socket_fromNative(C4SocketFactory factory,
                              void *nativeHandle,
                              const C4Address *address) C4API
{
    return tryCatch<C4Socket*>(nullptr, [&]{
        return new C4SocketImpl(Address(*address).url(), Role::Server, {}, &factory, nativeHandle);
    });
}

void c4socket_gotHTTPResponse(C4Socket *socket, int status, C4Slice responseHeadersFleece) C4API {
    try {
        Headers headers(responseHeadersFleece);
        internal(socket)->gotHTTPResponse(status, headers);
    } catchForSocket(socket)
}

void c4socket_opened(C4Socket *socket) C4API {
    try {
        internal(socket)->onConnect();
    } catchForSocket(socket)
}

void c4socket_closeRequested(C4Socket *socket, int status, C4String message) {
    try {
        internal(socket)->onCloseRequested(status, message);
    } catchForSocket(socket)
}

void c4socket_closed(C4Socket *socket, C4Error error) C4API {
    alloc_slice message = c4error_getMessage(error);
    CloseStatus status {kUnknownError, error.code, message};
    if (error.code == 0) {
        status.reason = kWebSocketClose;
        status.code = kCodeNormal;
    } else if (error.domain == WebSocketDomain)
        status.reason = kWebSocketClose;
    else if (error.domain == POSIXDomain)
        status.reason = kPOSIXError;
    else if (error.domain == NetworkDomain)
        status.reason = kNetworkError;

    try {
        internal(socket)->onClose(status);
    } catch (const std::exception &x) {
        WarnError("Exception caught in c4Socket_closed: %s", x.what());
    }
}

void c4socket_completedWrite(C4Socket *socket, size_t byteCount) C4API {
    try{
        internal(socket)->onWriteComplete(byteCount);
    } catchForSocket(socket)
}

void c4socket_received(C4Socket *socket, C4Slice data) C4API {
    try {
        internal(socket)->onReceive(data);
    } catchForSocket(socket)
}
