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

#include "Database.hh"
#include "c4Replicator.hh"
#include "c4ExceptionUtils.hh"
#include "DatabaseCookies.hh"
#include "StringUtil.hh"
#include <atomic>
#include <errno.h>


CBL_CORE_API const char* const kC4ReplicatorActivityLevelNames[5] = {
    "stopped", "offline", "connecting", "idle", "busy"
};


static bool isValidScheme(C4Slice scheme) {
    static const slice kValidSchemes[] = {"ws"_sl, "wss"_sl, "blip"_sl, "blips"_sl};
    for (int i=0; i < sizeof(kValidSchemes)/sizeof(slice); i++)
        if ((slice)scheme == kValidSchemes[i])
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
        address->port = (uint16_t)port;
    } else {
        colon = pathStart;
    }
    address->hostname = slice(str.buf, colon);

    if (pathStart >= str.end())
        return false;
    str.setStart(pathStart + 1);

    if (str.hasSuffix("/"_sl))
        str.setSize(str.size - 1);
    const uint8_t *slash;
    while ((slash = str.findByte('/')) != nullptr)
        str.setStart(slash + 1);

    address->path = slice(pathStart, str.buf);
    *dbName = str;
    return c4repl_isValidDatabaseName(toc4slice(str));
}


C4Replicator* c4repl_new(C4Database* db,
                         C4Address serverAddress,
                         C4String remoteDatabaseName,
                         C4Database* otherLocalDB,
                         C4ReplicatorParameters params,
                         C4Error *outError) C4API
{
    try {
        if (!checkParam(params.push != kC4Disabled || params.pull != kC4Disabled,
                        "Either push or pull must be enabled", outError))
            return nullptr;

        c4::ref<C4Database> dbCopy(c4db_openAgain(db, outError));
        if (!dbCopy)
            return nullptr;

        Retained<C4Replicator> replicator;
        if (otherLocalDB) {
            if (!checkParam(otherLocalDB != db, "Can't replicate a database to itself", outError))
                return nullptr;
            // Local-to-local:
            c4::ref<C4Database> otherDBCopy(c4db_openAgain(otherLocalDB, outError));
            if (!otherDBCopy)
                return nullptr;
            replicator = new C4Replicator(dbCopy, otherDBCopy, params);
        } else {
            // Remote:
            if (!checkParam(isValidScheme(serverAddress.scheme),
                            "Unsupported replication URL scheme", outError))
                return nullptr;
            replicator = new C4Replicator(dbCopy, serverAddress, remoteDatabaseName, params);
        }
        replicator->start();
        return retain((C4Replicator*)replicator);   // to be balanced by release in c4repl_free()
    } catchError(outError);
    return nullptr;
}


C4Replicator* c4repl_newWithSocket(C4Database* db,
                                   C4Socket *openSocket,
                                   C4ReplicatorParameters params,
                                   C4Error *outError) C4API
{
    try {
        c4::ref<C4Database> dbCopy(c4db_openAgain(db, outError));
        if (!dbCopy)
            return nullptr;
        C4Replicator *replicator = new C4Replicator(dbCopy, openSocket, params);
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
    return toc4slice(repl->responseHeaders().data());
}


#pragma mark - COOKIES:


C4StringResult c4db_getCookies(C4Database *db,
                               C4Address request,
                               C4Error *outError) C4API
{
    return tryCatch<C4StringResult>(outError, [=]() {
        DatabaseCookies cookies(db);
        string result = cookies.cookiesForRequest(addressFrom(request));
        if (result.empty()) {
            clearError(outError);
            return C4StringResult();
        }
        return sliceResult(result);
    });
}


bool c4db_setCookie(C4Database *db,
                    C4String setCookieHeader,
                    C4String fromHost,
                    C4Error *outError) C4API
{
    return tryCatch<bool>(outError, [=]() {
        DatabaseCookies cookies(db);
        bool ok = cookies.setCookie(slice(setCookieHeader).asString(),
                                    slice(fromHost).asString());
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

