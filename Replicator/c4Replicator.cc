//
//  c4Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Replicator.h"
#include "c4Socket+Internal.hh"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "StringUtil.hh"
#include <atomic>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


const char* const kC4ReplicatorActivityLevelNames[5] = {
    "stopped", "offline", "connecting", "idle", "busy"
};


static websocket::Address addressFrom(const C4Address &addr, C4String remoteDatabaseName) {
    return websocket::Address(asstring(addr.scheme),
                              asstring(addr.hostname),
                              addr.port,
                              format("/%.*s/_blipsync", SPLAT(remoteDatabaseName)));
}


static websocket::Address addressFrom(C4Database* otherDB) {
    alloc_slice path(c4db_getPath(otherDB));
    return websocket::Address("file", "", 0, path.asString());
}


struct C4Replicator : public RefCounted, Replicator::Delegate {

    // Constructor for replication with remote database
    C4Replicator(C4Database* db,
                 C4Address remoteAddress,
                 C4String remoteDatabaseName,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :C4Replicator(db, DefaultProvider(), addressFrom(remoteAddress, remoteDatabaseName),
                  push, pull, onStateChanged, callbackContext)
    { }

    // Constructor for replication with local database
    C4Replicator(C4Database* db,
                 C4Database* otherDB,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :C4Replicator(db, loopbackProvider(), addressFrom(otherDB),
                  push, pull, onStateChanged, callbackContext)
    {
        _otherDB = c4db_retain(otherDB);
        auto provider = loopbackProvider();
        auto dbAddr = addressFrom(db);
        _otherReplicator = new Replicator(otherDB,
                                          provider.createWebSocket(dbAddr), dbAddr,
                                          *this, { kC4Passive, kC4Passive });
        provider.connect(_replicator->webSocket(), _otherReplicator->webSocket());
    }


    C4ReplicatorStatus status() const   {return _status;}

    void stop()                         {_replicator->stop();}

    void detach()                       {_onStateChanged = nullptr;}

private:

    C4Replicator(C4Database* db,
                 websocket::Provider &provider,
                 websocket::Address address,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :_db(c4db_retain(db))
    ,_onStateChanged(onStateChanged)
    ,_callbackContext(callbackContext)
    ,_replicator(new Replicator(db, provider, address, *this, { push, pull }))
    ,_status(_replicator->status())
    {
        retain(this); // keep myself alive till replicator closes
    }

    ~C4Replicator() {
        c4db_free(_db);
        c4db_free(_otherDB);
    }

    static LoopbackProvider& loopbackProvider() {
        static LoopbackProvider sProvider;
        return sProvider;
    }

    virtual void replicatorStatusChanged(Replicator*,
                                         const Replicator::Status &newStatus) override
    {
        auto oldLevel = _status.load().level;
        _status = newStatus;
        notify();
        if (oldLevel != kC4Stopped && newStatus.level == kC4Stopped)
            release(this); // balances retain in constructor
    }

    void notify() {
        C4ReplicatorStatusChangedCallback on = _onStateChanged;
        if (on)
            on(this, _status, _callbackContext);
    }

    C4Database* _db {nullptr};
    C4Database* _otherDB {nullptr};
    atomic<C4ReplicatorStatusChangedCallback> _onStateChanged;
    void *_callbackContext;
    Retained<Replicator> _replicator;
    Retained<Replicator> _otherReplicator;
    atomic<C4ReplicatorStatus> _status;
};


static bool isValidScheme(C4Slice scheme) {
    static const slice kValidSchemes[] = {"ws"_sl, "wss"_sl, "blip"_sl, "blips"_sl};
    for (int i=0; i < sizeof(kValidSchemes)/sizeof(slice); i++)
        if (scheme == kValidSchemes[i])
            return true;
    return false;
}


static void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error *outError) {
    if (outError)
        *outError = c4error_make(domain, code, message);
}


static C4Replicator* returnInvalid(C4String message, C4Error *outError) {
    c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, message, outError);
    return nullptr;
}


C4Replicator* c4repl_new(C4Database* db,
                         C4Address serverAddress,
                         C4String remoteDatabaseName,
                         C4Database* otherLocalDB,
                         C4ReplicatorMode push,
                         C4ReplicatorMode pull,
                         C4ReplicatorStatusChangedCallback onStatusChanged,
                         void *callbackContext,
                         C4Error *outError) C4API
{
    try {
        if (push == kC4Disabled && pull == kC4Disabled)
            return returnInvalid("Either push or pull must be enabled"_sl, outError);

        c4::ref<C4Database> dbCopy(c4db_openAgain(db, outError));
        if (!dbCopy)
            return nullptr;

        C4Replicator *replicator;
        if (otherLocalDB) {
            if (otherLocalDB == db)
                return returnInvalid("Can't replicate a database to itself"_sl, outError);
            // Local-to-local:
            c4::ref<C4Database> otherDBCopy(c4db_openAgain(otherLocalDB, outError));
            if (!otherDBCopy)
                return nullptr;
            replicator = new C4Replicator(dbCopy, otherDBCopy,
                                          push, pull, onStatusChanged, callbackContext);
        } else {
            // Remote:
            if (!isValidScheme(serverAddress.scheme))
                return returnInvalid("Unsupported replication URL scheme"_sl, outError);
            replicator = new C4Replicator(dbCopy, serverAddress, remoteDatabaseName,
                                          push, pull, onStatusChanged, callbackContext);
        }
        return retain(replicator);
    } catch (const std::exception &x) {
        // TODO: Return a better error
        c4error_return(LiteCoreDomain, kC4ErrorUnexpectedError, slice(x.what()), outError);
        return nullptr;
    }
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
