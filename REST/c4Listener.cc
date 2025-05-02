//
// c4Listener.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Listener.hh"
#include "c4ListenerInternal.hh"
#include "c4ExceptionUtils.hh"
#include "c4Database.hh"
#include "c4Collection.hh"
#include "Address.hh"
#include "HTTPListener.hh"
#include "SyncListener.hh"
#include <algorithm>
#include <sstream>

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::REST;

namespace litecore::REST {
    C4LogDomain ListenerLog;
}  // namespace litecore::REST

string C4Listener::URLNameFromPath(slice pathSlice) {
    return DatabaseRegistry::databaseNameFromPath(FilePath(pathSlice, ""));
}

namespace {
    std::stringstream& operator<<(std::stringstream& ss, const C4ListenerConfig& config) {
        ss << "{"
           << "networkInterface: "
           << (!config.networkInterface            ? "NULL"
               : config.networkInterface.size == 0 ? "\"\""
                                                   : std::string(config.networkInterface))
           << ", "
           << "port: " << config.port << ", "
           << "tlsConfig: "
           << "{";
        if ( config.tlsConfig != nullptr ) {
            ss << "privateKeyRepresentation: "
               << (config.tlsConfig->privateKeyRepresentation == kC4PrivateKeyFromCert ? "PrivateKeyFromCert"
                                                                                       : "PrivateKeyFromKey")
               << ", "
               << "key: " << (config.tlsConfig->key == nullptr ? "NULL" : "***") << ", "
               << "certificate: " << (config.tlsConfig->certificate == nullptr ? "NULL" : "***") << ", "
               << "requireClientCerts: " << config.tlsConfig->requireClientCerts << ", "
               << "rootClientCerts: " << (config.tlsConfig->rootClientCerts == nullptr ? "NULL" : "***") << ", "
               << "certAuthCallback: " << (config.tlsConfig->certAuthCallback == nullptr ? "NULL" : "***") << ", "
               << "tlsCallbackContext: " << (config.tlsConfig->tlsCallbackContext == nullptr ? "NULL" : "***");
        }
        ss << "}, "
           << "httpAuthCallback: " << (config.httpAuthCallback == nullptr ? "NULL" : "***") << ", "
           << "callbackContext: " << (config.callbackContext == nullptr ? "NULL" : "***") << ", "
           << "allowPush: " << config.allowPush << ", "
           << "allowPull: " << config.allowPull << ", "
           << "enableDeltaSync: " << config.enableDeltaSync;
        ss << "}";
        return ss;
    }
}  // namespace

C4Listener::C4Listener(C4ListenerConfig const& config, Retained<HTTPListener> impl) : _impl(std::move(impl)) {
    _impl->setDelegate(this);
    std::stringstream ss;
    ss << config;
    c4log(ListenerLog, kC4LogInfo, "Listener config: %s", ss.str().c_str());
}

C4Listener::C4Listener(C4ListenerConfig const& config) : C4Listener(config, make_retained<SyncListener>(config)) {}

C4Listener::C4Listener(C4Listener&&) noexcept = default;

C4Listener::~C4Listener() noexcept { stop(); }

C4Error C4Listener::stop() noexcept {
    C4Error result{};
    if ( _impl ) {
        try {
            _impl->stop();
            _impl = nullptr;
        }
        catchError(&result);
    }
    return result;
}

bool C4Listener::shareDB(slice name, C4Database* db, C4ListenerDatabaseConfig const* dbConfig) {
    optional<string> nameStr;
    if ( name.buf ) nameStr = name;
    // `registerDatabase` now takes over that C4Database for the use of the listener.
    // That isn't how this API is defined, so to stay compatible, open a new connection to register:
    auto dbCopy = db->openAgain();
    try {
        if ( _impl->registerDatabase(db, nameStr, dbConfig) ) return true;
        dbCopy->close();
        return false;
    } catch ( ... ) {
        dbCopy->close();
        throw;
    }
}

bool C4Listener::unshareDB(C4Database* db) { return _impl->unregisterDatabase(db); }

bool C4Listener::shareCollection(slice name, C4Collection* coll) {
    if ( _usuallyFalse(!coll || !coll->isValid()) ) {
        C4Error::raise(LiteCoreDomain, kC4ErrorNotOpen, "Invalid collection: either deleted, or db closed");
    }
    return _impl->registerCollection((string)name, coll->getSpec());
}

bool C4Listener::unshareCollection(slice name, C4Collection* coll) {
    return _impl->unregisterCollection((string)name, coll->getSpec());
}

std::vector<std::string> C4Listener::URLs(C4Database* C4NULLABLE db) const {
    vector<string> urls;
    for ( net::Address& address : _impl->addresses(db, true) ) urls.emplace_back(string(address.url()));
    return urls;
}

uint16_t C4Listener::port() const { return _impl->port(); }

std::pair<unsigned, unsigned> C4Listener::connectionStatus() const {
    auto active                = _impl->activeConnectionCount();
    auto connectionCount       = std::max(_impl->connectionCount(), active);
    auto activeConnectionCount = active;
    return {connectionCount, activeConnectionCount};
}

#endif  // COUCHBASE_ENTERPRISE
