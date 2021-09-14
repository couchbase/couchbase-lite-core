//
// c4Replicator.hh
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
#include "c4ReplicatorTypes.h"

C4_ASSUME_NONNULL_BEGIN


// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


struct C4Replicator : public fleece::RefCounted,
                      public fleece::InstanceCountedIn<C4Replicator>,
                      C4Base
{
    // NOTE: Instances are created with database->newReplicator(...).

    static bool isValidDatabaseName(slice dbName) noexcept;

    static void validateRemote(const C4Address &addr,
                               slice dbName);

    virtual void start(bool reset =false) noexcept =0;
    virtual void stop() noexcept =0;
    bool retry();

    virtual void stopCallbacks() noexcept =0;

    virtual void setHostReachable(bool) noexcept { }
    virtual void setSuspended(bool) noexcept =0;

    void setOptions(slice optionsDictFleece);
    virtual void setProgressLevel(C4ReplicatorProgressLevel) noexcept =0;

    virtual C4ReplicatorStatus getStatus() const noexcept =0;
    virtual alloc_slice getResponseHeaders() const noexcept =0;

    alloc_slice pendingDocIDs() const;
    bool isDocumentPending(slice docID) const;

#ifdef COUCHBASE_ENTERPRISE
    C4Cert* C4NULLABLE getPeerTLSCertificate() const;
#endif
};

C4_ASSUME_NONNULL_END
