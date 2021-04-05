//
// c4Listener.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once
#include "c4Base.hh"
#include "c4ListenerTypes.h"
#include <vector>

C4_ASSUME_NONNULL_BEGIN


// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


struct C4Listener final : public fleece::InstanceCounted, C4Base {

    static C4ListenerAPIs availableAPIs();

    explicit C4Listener(C4ListenerConfig config);

    ~C4Listener();

    bool shareDB(slice name, C4Database *db);

    bool unshareDB(C4Database *db);

    uint16_t port();

    std::pair<unsigned, unsigned> connectionStatus();

    std::vector<std::string> URLs(C4Database* C4NULLABLE db, C4ListenerAPIs api);

    static std::string URLNameFromPath(slice path);

private:
    C4Listener(const C4Listener&) = delete;
    C4Listener(C4Listener&&);
    litecore::REST::RESTListener* restImpl();

    Retained<litecore::REST::Listener> _impl;
    C4ListenerHTTPAuthCallback C4NULLABLE _httpAuthCallback;
    void* C4NULLABLE _callbackContext;
};


C4_ASSUME_NONNULL_END
