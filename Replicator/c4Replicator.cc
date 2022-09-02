//
// c4Replicator.cc
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

using namespace std;
using namespace litecore;


#pragma mark - C++ API:


// All instances are subclasses of C4BaseReplicator.
static C4ReplicatorImpl* asInternal(const C4Replicator *repl) {return (C4ReplicatorImpl*)repl;}


Retained<C4Replicator> C4Database::newReplicator(C4Address serverAddress,
                                                 slice remoteDatabaseName,
                                                 const C4ReplicatorParameters &params)
{
    std::for_each(params.collections, params.collections + params.collectionCount,
                      [](const C4ReplicationCollection& coll) {
        AssertParam(coll.push != kC4Disabled || coll.pull != kC4Disabled,
                    "Either push or pull must be enabled");
    });

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
    std::for_each(params.collections, params.collections + params.collectionCount,
                  [](const C4ReplicationCollection& coll) {
        AssertParam(coll.push != kC4Disabled || coll.pull != kC4Disabled,
                    "Either push or pull must be enabled");
    });
    AssertParam(otherLocalDB != this, "Can't replicate a database to itself");
    return new C4LocalReplicator(this, params, otherLocalDB);
}
#endif


Retained<C4Replicator> C4Database::newIncomingReplicator(WebSocket *openSocket,
                                                         const C4ReplicatorParameters &params)
{
    return new C4IncomingReplicator(this, params, openSocket);
}


Retained<C4Replicator> C4Database::newIncomingReplicator(C4Socket *openSocket,
                                                         const C4ReplicatorParameters &params)
{
    return newIncomingReplicator(WebSocketFrom(openSocket), params);
}


bool C4Replicator::retry() {
    return asInternal(this)->retry(true);
}

void C4Replicator::setOptions(slice optionsDictFleece) {
    asInternal(this)->setProperties(AllocedDict(optionsDictFleece));
}

alloc_slice C4Replicator::pendingDocIDs(C4CollectionSpec spec) const {
    return asInternal(this)->pendingDocumentIDs(spec);
}

bool C4Replicator::isDocumentPending(slice docID, C4CollectionSpec spec) const {
    return asInternal(this)->isDocumentPending(docID, spec);
}

#ifdef COUCHBASE_ENTERPRISE
C4Cert* C4Replicator::getPeerTLSCertificate() const {
    return asInternal(this)->getPeerTLSCertificate();
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
        && !dbName.findByteNotIn("abcdefghijklmnopqrstuvwxyz0123456789_$()+-/"_sl);
}


bool C4Address::isValidRemote(slice dbName, C4Error *outError) const noexcept {
    slice message;
    if (!isValidReplicatorScheme(scheme))
        message = "Invalid replication URL scheme (use ws: or wss:)"_sl;
    else if (!C4Replicator::isValidDatabaseName(dbName))
        message = "Invalid or missing remote database name"_sl;
    else if (hostname.size == 0 || port == 0)
        message = "Invalid replication URL (bad hostname or port)"_sl;

    if (message) {
        c4error_return(NetworkDomain, kC4NetErrInvalidURL, message, outError);
        return false;
    }
    return true;
}


void C4Replicator::validateRemote(const C4Address &addr, slice dbName) {
    C4Error error;
    if (!addr.isValidRemote(dbName, &error))
        C4Error::raise(error);
}


bool C4Address::fromURL(slice url, C4Address *address, slice *dbName) {
    slice str = url;

    auto colon = str.findByteOrEnd(':');
    if (!colon)
        return false;
    address->scheme = slice(str.buf, colon);
    if (!isValidScheme(address->scheme))
        return false;
    address->port = defaultPortForScheme(address->scheme);
    str.setStart(colon);
    if (!str.hasPrefix("://"_sl))
        return false;
    str.moveStart(3);

    if (str.size > 0 && str[0] == '[') {
        // IPv6 address in URL is bracketed (RFC 2732):
        auto endBr = str.findByte(']');
        if (!endBr)
            return false;
        address->hostname = slice(&str[1], endBr);
        if (address->hostname.size == 0)
            return false;
        str.setStart(endBr + 1);
    } else {
        address->hostname = nullslice;
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
        address->port = (uint16_t)port;
    } else {
        colon = pathStart;
    }
    if (!address->hostname.buf) {
        address->hostname = slice(str.buf, colon);
        if (address->hostname.size == 0)
            address->port = 0;
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

        address->path = slice(pathStart, str.buf);
        *dbName = str;
        return C4Replicator::isValidDatabaseName(str);
    } else {
        address->path = slice(pathStart, str.end());
        return true;
    }
}


alloc_slice C4Address::toURL() const {
    stringstream s;
    s << scheme << "://";
    if (slice(hostname).findByte(':'))
        s << '[' << hostname << ']';
    else
        s << hostname;
    if (port)
        s << ':' << port;
    if (path.size == 0 || slice(path)[0] != '/')
        s << '/';
    s << path;
    return alloc_slice(s.str());
}
