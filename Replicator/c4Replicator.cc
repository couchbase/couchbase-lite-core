//
// c4Replicator.cc
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

#include "c4Replicator.hh"
#include "c4RemoteReplicator.hh"
#ifdef COUCHBASE_ENTERPRISE
#include "c4LocalReplicator.hh"
#endif
#include "c4IncomingReplicator.hh"
#include "c4Database.hh"
#include "c4ExceptionUtils.hh"
#include "DatabaseCookies.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include <errno.h>

using namespace c4Internal;
using namespace std;


#pragma mark - C++ API:


// All instances are subclasses of C4BaseReplicator.
static C4ReplicatorImpl* asInternal(const C4Replicator *repl) {return (C4ReplicatorImpl*)repl;}


Retained<C4Replicator> C4Database::newReplicator(C4Address serverAddress,
                                                 slice remoteDatabaseName,
                                                 const C4ReplicatorParameters &params)
{
    AssertParam(params.push != kC4Disabled || params.pull != kC4Disabled,
                "Either push or pull must be enabled");
    if (!params.socketFactory) {
        C4Replicator::validateRemote(serverAddress, remoteDatabaseName);
        if (serverAddress.port == 4985 && serverAddress.hostname != "localhost"_sl) {
            Warn("POSSIBLE SECURITY ISSUE: It looks like you're connecting to Sync Gateway's "
                 "admin port (4985) -- this is usually a bad idea. By default this port is "
                 "unreachable, but if opened, it would give anyone unlimited privileges.");
        }
    }
    return new C4RemoteReplicator(this, params, serverAddress, remoteDatabaseName);
}


#ifdef COUCHBASE_ENTERPRISE
Retained<C4Replicator> C4Database::newLocalReplicator(C4Database *otherLocalDB,
                                                      const C4ReplicatorParameters &params)
{
    AssertParam(params.push != kC4Disabled || params.pull != kC4Disabled,
                "Either push or pull must be enabled");
    AssertParam(otherLocalDB != this, "Can't replicate a database to itself");
    return new C4LocalReplicator(this, params, otherLocalDB);
}
#endif


Retained<C4Replicator> C4Database::newReplicator(WebSocket *openSocket,
                                                 const C4ReplicatorParameters &params)
{
    return new C4IncomingReplicator(this, params, openSocket);
}


Retained<C4Replicator> C4Database::newReplicator(C4Socket *openSocket,
                                   const C4ReplicatorParameters &params)
{
    return newReplicator(WebSocketFrom(openSocket), params);
}


void C4Replicator::retry() {
    C4Error error;
    if (!asInternal(this)->retry(true, &error))
        throwError(error);
}

void C4Replicator::setOptions(slice optionsDictFleece) {
    asInternal(this)->setProperties(AllocedDict(optionsDictFleece));
}

alloc_slice C4Replicator::pendingDocIDs() const {
    C4Error error;
    alloc_slice ids = asInternal(this)->pendingDocumentIDs(&error);
    if (!ids)
        throwError(error);
    return ids;
}

bool C4Replicator::isDocumentPending(slice docID) const {
    C4Error error;
    if (asInternal(this)->isDocumentPending(docID, &error))
        return true;
    else if (error.code == 0)
        return false;
    else
        throwError(error);
}

#ifdef COUCHBASE_ENTERPRISE
C4Cert* C4Replicator::peerTLSCertificate() const {
    C4Error error;
    auto cert = asInternal(this)->getPeerTLSCertificate(&error);
    if (!cert && error.code)
        throwError(error);
    return cert;
}
#endif


CBL_CORE_API const char* const kC4ReplicatorActivityLevelNames[6] = {
    "stopped", "offline", "connecting", "idle", "busy", "stopping"
};


static bool isValidScheme(slice scheme) {
    return scheme.size > 0 && isalpha(scheme[0]);
}


static bool isValidReplicatorScheme(slice scheme) {
    const slice kValidSchemes[] = {kC4Replicator2Scheme, kC4Replicator2TLSScheme, nullslice};
    for (int i=0; kValidSchemes[i]; ++i)
        if (scheme.caseEquivalent(kValidSchemes[i]))
            return true;
    return false;
}


static uint16_t defaultPortForScheme(slice scheme) {
    if (scheme.caseEquivalent("ws"_sl) || scheme[scheme.size-1] != 's')
        return 80;
    else
        return 443;
}


bool C4Replicator::isValidDatabaseName(slice dbName) noexcept {
    // Same rules as Couchbase Lite 1.x and CouchDB
    return dbName.size > 0 && dbName.size < 240
        && islower(dbName[0])
        && !slice(dbName).findByteNotIn("abcdefghijklmnopqrstuvwxyz0123456789_$()+-/"_sl);
}


bool C4Replicator::isValidRemote(const C4Address &addr, slice dbName, C4Error *outError) noexcept {
    slice message;
    if (!isValidReplicatorScheme(addr.scheme))
        message = "Invalid replication URL scheme (use ws: or wss:)"_sl;
    else if (!c4repl_isValidDatabaseName(dbName))
        message = "Invalid or missing remote database name"_sl;
    else if (addr.hostname.size == 0 || addr.port == 0)
        message = "Invalid replication URL (bad hostname or port)"_sl;

    if (message) {
        c4error_return(NetworkDomain, kC4NetErrInvalidURL, message, outError);
        return false;
    }
    return true;
}


void C4Replicator::validateRemote(const C4Address &addr, slice dbName) {
    C4Error error;
    if (!isValidRemote(addr, dbName, &error))
        throwError(error);
}


bool C4Replicator::addressFromURL(slice url, C4Address &address, slice *dbName) {
    slice str = url;

    auto colon = str.findByteOrEnd(':');
    if (!colon)
        return false;
    address.scheme = slice(str.buf, colon);
    if (!isValidScheme(address.scheme))
        return false;
    address.port = defaultPortForScheme(address.scheme);
    str.setStart(colon);
    if (!str.hasPrefix("://"_sl))
        return false;
    str.moveStart(3);

    if (str.size > 0 && str[0] == '[') {
        // IPv6 address in URL is bracketed (RFC 2732):
        auto endBr = str.findByte(']');
        if (!endBr)
            return false;
        address.hostname = slice(&str[1], endBr);
        if (address.hostname.size == 0)
            return false;
        str.setStart(endBr + 1);
    } else {
        address.hostname = nullslice;
    }

    colon = str.findByteOrEnd(':');
    auto pathStart = str.findByteOrEnd('/');
    if (str.findByteOrEnd('@') < pathStart)
        return false;                               // No usernames or passwords allowed!
    if (colon < pathStart) {
        int port;
        try {
            port = stoi(slice(colon+1, pathStart).asString());
        } catch (...) {
            return false;
        }
        if (port < 0 || port > 65535)
            return false;
        address.port = (uint16_t)port;
    } else {
        colon = pathStart;
    }
    if (!address.hostname.buf) {
        address.hostname = slice(str.buf, colon);
        if (address.hostname.size == 0)
            address.port = 0;
    }

    if (dbName) {
        if (pathStart >= str.end())
            return false;

        str.setStart(pathStart + 1);

        if (str.hasSuffix("/"_sl))
            str.setSize(str.size - 1);
        const uint8_t *slash;
        while ((slash = str.findByte('/')) != nullptr)
            str.setStart(slash + 1);

        address.path = slice(pathStart, str.buf);
        *dbName = str;
        return c4repl_isValidDatabaseName(slice(str));
    } else {
        address.path = slice(pathStart, str.end());
        return true;
    }
}


alloc_slice C4Replicator::addressToURL(const C4Address &address) {
    stringstream s;
    s << address.scheme << "://";
    if (slice(address.hostname).findByte(':'))
        s << '[' << address.hostname << ']';
    else
        s << address.hostname;
    if (address.port)
        s << ':' << address.port;
    if (address.path.size == 0 || slice(address.path)[0] != '/')
        s << '/';
    s << address.path;
    return alloc_slice(s.str());
}


#pragma mark - C API:


bool c4repl_isValidDatabaseName(C4String dbName) C4API {
    return C4Replicator::isValidDatabaseName(dbName);
}


bool c4repl_isValidRemote(C4Address addr, C4String dbName, C4Error *outError) C4API {
    return C4Replicator::isValidRemote(addr, dbName, outError);
}


bool c4address_fromURL(C4String url, C4Address *address, C4String *dbName) C4API {
    return C4Replicator::addressFromURL(url, *address, (slice*)dbName);
}


C4StringResult c4address_toURL(C4Address address) C4API {
    try {
        return C4StringResult(C4Replicator::addressToURL(address));
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
        if (!checkParam(params.push != kC4Disabled || params.pull != kC4Disabled,
                        "Either push or pull must be enabled", outError))
            return nullptr;

        if (!params.socketFactory) {
            if (!c4repl_isValidRemote(serverAddress, remoteDatabaseName, outError))
                return nullptr;
            if (serverAddress.port == 4985 && serverAddress.hostname != "localhost"_sl) {
                Warn("POSSIBLE SECURITY ISSUE: It looks like you're connecting to Sync Gateway's "
                     "admin port (4985) -- this is usually a bad idea. By default this port is "
                     "unreachable, but if opened, it would give anyone unlimited privileges.");
            }
        }
        return retain(new C4RemoteReplicator(db, params, serverAddress, remoteDatabaseName));
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
        if (!checkParam(params.push != kC4Disabled || params.pull != kC4Disabled,
                        "Either push or pull must be enabled", outError))
            return nullptr;
        if (!checkParam(otherLocalDB != db, "Can't replicate a database to itself", outError))
            return nullptr;

        return retain(new C4LocalReplicator(db, params, otherLocalDB));
    } catchError(outError);
    return nullptr;
}
#endif

C4Replicator* c4repl_newWithWebSocket(C4Database* db,
                                      WebSocket *openSocket,
                                      C4ReplicatorParameters params,
                                      C4Error *outError) C4API
{
    try {
        return retain(new C4IncomingReplicator(db, params, openSocket));
    } catchError(outError);
    return nullptr;
}


C4Replicator* c4repl_newWithSocket(C4Database* db,
                                   C4Socket *openSocket,
                                   C4ReplicatorParameters params,
                                   C4Error *outError) C4API
{
    return c4repl_newWithWebSocket(db, WebSocketFrom(openSocket), params, outError);
}


void c4repl_start(C4Replicator* repl, bool reset) C4API {
    repl->start(reset);
}


void c4repl_stop(C4Replicator* repl) C4API {
    repl->stop();
}


bool c4repl_retry(C4Replicator* repl, C4Error *outError) C4API {
    return tryCatch<bool>(nullptr, [&] {
        return asInternal(repl)->retry(true, outError);
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
    asInternal(repl)->detach();
    release(repl);
}


C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl) C4API {
    return repl->status();
}


C4Slice c4repl_getResponseHeaders(C4Replicator *repl) C4API {
    return repl->responseHeaders();
}


C4SliceResult c4repl_getPendingDocIDs(C4Replicator* repl, C4Error* outErr) C4API {
    try {
        return C4SliceResult( asInternal(repl)->pendingDocumentIDs(outErr) );
    } catchError(outErr);
    return {};
}


bool c4repl_isDocumentPending(C4Replicator* repl, C4Slice docID, C4Error* outErr) C4API {
    try {
        return asInternal(repl)->isDocumentPending(docID, outErr);
    } catchError(outErr);
    return false;
}
    
C4Cert* c4repl_getPeerTLSCertificate(C4Replicator* repl, C4Error* outErr) C4API {
#ifdef COUCHBASE_ENTERPRISE
    outErr->code = 0;
    return asInternal(repl)->getPeerTLSCertificate(outErr);
#else
    outErr->domain = LiteCoreDomain;
    outErr->code = kC4ErrorUnsupported;
    return nullptr;
#endif
}


bool c4repl_setProgressLevel(C4Replicator* repl, C4ReplicatorProgressLevel level, C4Error* outErr) C4API {
    C4_START_WARNINGS_SUPPRESSION
    C4_IGNORE_TAUTOLOGICAL
    
    if(_usuallyFalse(repl == nullptr)) {
        if(outErr) {
            *outErr = c4error_make(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("repl was null"));
        }

        return false;
    }
    
    C4_STOP_WARNINGS_SUPPRESSION

    if(_usuallyFalse(level < kC4ReplProgressOverall || level > kC4ReplProgressPerAttachment)) {
        if(outErr) {
            *outErr = c4error_make(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("level out of range"));
        }

        return false;
    }

    repl->setProgressLevel(level);
    return true;
}


#pragma mark - COOKIES:

#include "c4ExceptionUtils.hh"
using namespace c4Internal;


C4StringResult c4db_getCookies(C4Database *db,
                               C4Address request,
                               C4Error *outError) C4API
{
    return tryCatch<C4StringResult>(outError, [=]() {
        DatabaseCookies cookies(db);
        string result = cookies.cookiesForRequest(request);
        if (result.empty()) {
            clearError(outError);
            return C4StringResult();
        }
        return FLSliceResult(alloc_slice(result));
    });
}


bool c4db_setCookie(C4Database *db,
                    C4String setCookieHeader,
                    C4String fromHost,
                    C4String fromPath,
                    C4Error *outError) C4API
{
    return tryCatch<bool>(outError, [=]() {
        DatabaseCookies cookies(db);
        bool ok = cookies.setCookie(slice(setCookieHeader).asString(),
                                    slice(fromHost).asString(),
                                    slice(fromPath).asString());
        if (ok)
            cookies.saveChanges();
        else
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, C4STR("Invalid cookie"), outError);
        return ok;
    });
}


void c4db_clearCookies(C4Database *db) C4API {
    tryCatch(nullptr, [db]() {
        DatabaseCookies cookies(db);
        cookies.clearCookies();
        cookies.saveChanges();
    });
}

