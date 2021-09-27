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
#include "Listener.hh"
#include "RESTListener.hh"
#include "fleece/Mutable.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::REST;


namespace litecore { namespace REST {
    C4LogDomain ListenerLog;
} }


C4ListenerAPIs C4Listener::availableAPIs() {
    return kListenerAPIs;
}


string C4Listener::URLNameFromPath(slice pathSlice) {
    return Listener::databaseNameFromPath(FilePath(pathSlice, ""));
}


C4Listener::C4Listener(C4ListenerConfig config)
:_httpAuthCallback(config.httpAuthCallback)
,_callbackContext(config.callbackContext)
{
    // Replace the callback, if any, with one to myself. This allows me to pass the correct
    // C4Listener* to the client's callback.
    if (config.httpAuthCallback) {
        config.callbackContext = this;
        config.httpAuthCallback = [](C4Listener*, C4Slice authHeader, void *context) {
            auto listener = (C4Listener*)context;
            return listener->_httpAuthCallback(listener, authHeader, listener->_callbackContext);
        };
    }

    _impl = dynamic_cast<RESTListener*>(NewListener(&config).get());
    if (!_impl)
        C4Error::raise(LiteCoreDomain, kC4ErrorUnsupported, "Unsupported listener API");
}


C4Listener::C4Listener(C4Listener&&) = default;


C4Listener::~C4Listener() {
    if (_impl)
        _impl->stop();
}


bool C4Listener::shareDB(slice name, C4Database *db) {
    optional<string> nameStr;
    if (name.buf)
        nameStr = name;
    return _impl->registerDatabase(db, nameStr);
}


bool C4Listener::unshareDB(C4Database *db) {
    return _impl->unregisterDatabase(db);
}


std::vector<std::string> C4Listener::URLs(C4Database* C4NULLABLE db, C4ListenerAPIs api) const {
    AssertParam(api == kC4RESTAPI || api == kC4SyncAPI,
                "The provided API must be one of the following:  REST, Sync.");
    vector<string> urls;
    for (net::Address &address : _impl->addresses(db, api))
        urls.push_back(string(address.url()));
    return urls;
}


uint16_t C4Listener::port() const {
    return _impl->port();
}


std::pair<unsigned, unsigned> C4Listener::connectionStatus() const {
    auto active = _impl->activeConnectionCount();
    auto connectionCount = std::max(_impl->connectionCount(), active);
    auto activeConnectionCount = active;
    return {connectionCount, activeConnectionCount};
}
