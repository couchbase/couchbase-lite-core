//
// c4Listener.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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

    bool shareCollection(slice name, C4Collection* coll);

    bool unshareCollection(slice name, C4Collection* coll);

    uint16_t port() const;

    std::pair<unsigned, unsigned> connectionStatus() const;

    std::vector<std::string> URLs(C4Database* C4NULLABLE db, C4ListenerAPIs api) const;

    static std::string URLNameFromPath(slice path);

private:
    C4Listener(const C4Listener&) = delete;
    C4Listener(C4Listener&&);

    Retained<litecore::REST::RESTListener> _impl;
    C4ListenerHTTPAuthCallback C4NULLABLE _httpAuthCallback;
    void* C4NULLABLE _callbackContext;
};


C4_ASSUME_NONNULL_END
