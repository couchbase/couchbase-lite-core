//
//  c4Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4Replicator.hh"
#include "c4ExceptionUtils.hh"
#include "StringUtil.hh"
#include <atomic>
#include <errno.h>
#ifdef _MSC_VER
#include <winerror.h>
#define EHOSTDOWN WSAEHOSTDOWN
#endif


const char* const kC4ReplicatorActivityLevelNames[5] = {
    "stopped", "offline", "connecting", "idle", "busy"
};


static bool isValidScheme(C4Slice scheme) {
    static const slice kValidSchemes[] = {"ws"_sl, "wss"_sl, "blip"_sl, "blips"_sl};
    for (int i=0; i < sizeof(kValidSchemes)/sizeof(slice); i++)
        if (scheme == kValidSchemes[i])
            return true;
    return false;
}


bool c4repl_isValidDatabaseName(C4String dbName) {
    slice name = dbName;
    // Same rules as Couchbase Lite 1.x and CouchDB
    return name.size > 0 && name.size < 240
        && islower(name[0])
        && !slice(name).findByteNotIn("abcdefghijklmnopqrstuvwxyz0123456789_$()+-/"_sl);
}


bool c4repl_parseURL(C4String url, C4Address *address, C4String *dbName) {
    slice str = url;

    auto colon = str.findByteOrEnd(':');
    if (!colon)
        return false;
    address->scheme = slice(str.buf, colon);
    if (!isValidScheme(address->scheme))
        return false;
    address->port = (colon[-1] == 's') ? 443 : 80;
    str.setStart(colon);
    if (!str.hasPrefix("://"_sl))
        return false;
    str.moveStart(3);

    colon = str.findByteOrEnd(':');
    auto slash = str.findByteOrEnd('/');
    if (colon < slash) {
        int port;
        try {
            port = stoi(slice(colon+1, slash).asString());
        } catch (...) {
            return false;
        }
        if (port < 0 || port > 65535)
            return false;
        address->port = (uint16_t)port;
    } else {
        colon = slash;
    }
    address->hostname = slice(str.buf, colon);
    str.setStart(slash);
    if (str.size == 0)
        return false;
    address->path = "/"_sl;
    if (str[0] == '/')
        str.moveStart(1);
    if (str.hasSuffix("/"_sl))
        str.setSize(str.size - 1);
    *dbName = str;
    return c4repl_isValidDatabaseName(str);
}


C4Replicator* c4repl_new(C4Database* db,
                         C4Address serverAddress,
                         C4String remoteDatabaseName,
                         C4Database* otherLocalDB,
                         C4ReplicatorMode push,
                         C4ReplicatorMode pull,
                         C4Slice optionsDictFleece,
                         C4ReplicatorStatusChangedCallback onStatusChanged,
                         void *callbackContext,
                         C4Error *outError) C4API
{
    try {
        if (!checkParam(push != kC4Disabled || pull != kC4Disabled,
                        "Either push or pull must be enabled", outError))
            return nullptr;

        c4::ref<C4Database> dbCopy(c4db_openAgain(db, outError));
        if (!dbCopy)
            return nullptr;

        C4Replicator *replicator;
        if (otherLocalDB) {
            if (!checkParam(otherLocalDB != db, "Can't replicate a database to itself", outError))
                return nullptr;
            // Local-to-local:
            c4::ref<C4Database> otherDBCopy(c4db_openAgain(otherLocalDB, outError));
            if (!otherDBCopy)
                return nullptr;
            replicator = new C4Replicator(dbCopy, otherDBCopy,
                                          push, pull, optionsDictFleece,
                                          onStatusChanged, callbackContext);
        } else {
            // Remote:
            if (!checkParam(isValidScheme(serverAddress.scheme),
                            "Unsupported replication URL scheme", outError))
                return nullptr;
            replicator = new C4Replicator(dbCopy, serverAddress, remoteDatabaseName,
                                          push, pull, optionsDictFleece,
                                          onStatusChanged, callbackContext);
        }
        return retain(replicator);
    } catchError(outError);
    return nullptr;
}


C4Replicator* c4repl_newWithSocket(C4Database* db,
                                   C4Socket *openSocket,
                                   C4ReplicatorMode push,
                                   C4ReplicatorMode pull,
                                   C4Slice optionsDictFleece,
                                   C4ReplicatorStatusChangedCallback onStatusChanged,
                                   void *callbackContext,
                                   C4Error *outError) C4API
{
    try {
        c4::ref<C4Database> dbCopy(c4db_openAgain(db, outError));
        if (!dbCopy)
            return nullptr;
        C4Replicator *replicator = new C4Replicator(dbCopy, openSocket,
                                                    push, pull,
                                                    optionsDictFleece,
                                                    onStatusChanged, callbackContext);
        return retain(replicator);
    } catchError(outError);
    return nullptr;
}


void c4repl_stop(C4Replicator* repl) C4API {
    repl->stop();
}


void c4repl_free(C4Replicator* repl) C4API {
    if (!repl)
        return;
    repl->stop();
    repl->detach();
    release(repl);
}


C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl) C4API {
    return repl->status();
}


C4Slice c4repl_getResponseHeaders(C4Replicator *repl) C4API {
    return repl->responseHeaders().data();
}


#pragma mark - ERROR UTILITIES:


using CodeList = const int[];
using ErrorSet = const int* [kC4MaxErrorDomainPlus1];


static bool errorIsInSet(C4Error err, ErrorSet set) {
    if (err.code != 0 && (unsigned)err.domain < kC4MaxErrorDomainPlus1) {
        const int *pCode = set[err.domain];
        if (pCode) {
            for (; *pCode != 0; ++pCode)
                if (*pCode == err.code)
                    return true;
        }
    }
    return false;
}


bool c4error_mayBeTransient(C4Error err) C4API {
    static CodeList kTransientPOSIX = {
        ENETRESET, ECONNABORTED, ECONNRESET, ETIMEDOUT, ECONNREFUSED, 0};
    static CodeList kTransientNetwork = {
        kC4NetErrDNSFailure,
        0};
    static CodeList kTransientWebSocket = {
        408, /* Request Timeout */
        429, /* Too Many Requests (RFC 6585) */
        500, /* Internal Server Error */
        502, /* Bad Gateway */
        503, /* Service Unavailable */
        504, /* Gateway Timeout */
        kCodeGoingAway,
        0};
    static ErrorSet kTransient = { // indexed by C4ErrorDomain
        nullptr,
        nullptr,
        kTransientPOSIX,
        nullptr,
        nullptr,
        nullptr,
        kTransientNetwork,
        kTransientWebSocket};
    return errorIsInSet(err, kTransient);
}

bool c4error_mayBeNetworkDependent(C4Error err) C4API {
    static CodeList kUnreachablePOSIX = {
        ENETDOWN, ENETUNREACH, ETIMEDOUT, EHOSTDOWN, EHOSTUNREACH, 0};
    static CodeList kUnreachableNetwork = {
        kC4NetErrDNSFailure,
        kC4NetErrUnknownHost,   // Result may change if user logs into VPN or moves to intranet
        0};
    static ErrorSet kUnreachable = { // indexed by C4ErrorDomain
        nullptr,
        nullptr,
        kUnreachablePOSIX,
        nullptr,
        nullptr,
        nullptr,
        kUnreachableNetwork,
        nullptr};
    return errorIsInSet(err, kUnreachable);
}
