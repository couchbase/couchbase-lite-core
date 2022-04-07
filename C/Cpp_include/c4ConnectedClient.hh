//
// c4ConnectedClient.hh
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "Async.hh"
#include "c4ConnectedClient.h"

C4_ASSUME_NONNULL_BEGIN

struct C4ConnectedClient  : public fleece::RefCounted,
                            public fleece::InstanceCountedIn<C4Database>,
                            C4Base
{
    /// Creates a new ConnectedClient
    static Retained<C4ConnectedClient> newClient(const C4ConnectedClientParameters &params);
                    
    /// Gets the current revision of a document from the server.
    virtual litecore::actor::Async<C4DocResponse> getDoc(C4Slice, C4Slice, C4Slice, bool) noexcept=0;
    
    /// Pushes a new document revision to the server.
    /// @param docID  The document ID.
    /// @param collectionID  The name of the document's collection, or `nullslice` for default.
    /// @param revID The new revision ID
    /// @param parentRevID The ID of the parent revision on the server,
    ///                      or `nullslice` if this is a new document.
    /// @param revisionFlags  Flags of this revision.
    /// @param fleeceData  The document body encoded as Fleece (without shared keys!)
    /// @return  An async value that, when resolved, contains the status as a C4Error.
    virtual litecore::actor::Async<void> updateDoc(C4Slice docID,
                                                   C4Slice collectionID,
                                                   C4Slice revID,
                                                   C4Slice parentRevID,
                                                   C4RevisionFlags revisionFlags,
                                                   C4Slice fleeceData) noexcept=0;

    /// Tells a connected client to start.
    virtual void start() noexcept=0;

    /// Tells a replicator to stop.
    virtual void stop() noexcept=0;
};

C4_ASSUME_NONNULL_END
