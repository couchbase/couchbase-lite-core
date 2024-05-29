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
#include "c4ConnectedClientTypes.h"
#include "fleece/InstanceCounted.hh"
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN

struct C4ConnectedClient
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4Database>
    , C4Base {
    /// Creates a new ConnectedClient
    /// \note It will automatically starts the client, no need to call `start()`.
    static Retained<C4ConnectedClient> newClient(C4Database* db, const C4ConnectedClientParameters& params);

    /// Tells a connected client to start.
    virtual void start() = 0;

    /// Tells a replicator to stop.
    virtual void stop() = 0;

    /// The HTTP response headers.
    virtual alloc_slice getResponseHeaders() const noexcept = 0;

#ifdef COUCHBASE_ENTERPRISE
    /// The server's TLS certificate.
    virtual C4Cert* C4NULLABLE getPeerTLSCertificate() const = 0;
#endif

    /// The current connection status.
    virtual C4ConnectedClientStatus getStatus() const = 0;

    /// Gets the current revision of a document from the server.
    /// You can set the `unlessRevID` parameter to avoid getting a redundant copy of a
    /// revision you already have.
    virtual void getDoc(C4CollectionSpec const& collection, slice docID, slice unlessRevID, bool asFleece,
                        C4ConnectedClientGetDocumentCallback callback, void* C4NULLABLE context) = 0;

    /// Pushes a new document revision to the server.
    virtual void putDoc(C4CollectionSpec const&, slice docID, slice parentRevID, C4RevisionFlags revisionFlags,
                        slice fleeceData, C4ConnectedClientUpdateDocumentCallback callback,
                        void* C4NULLABLE context) = 0;

#if 0
    /// Callback for \ref getAllDocIDs.
    /// @param ids  A vector of docIDs; empty on the final call.
    /// @param error  NULL or a pointer to an error.
    using AllDocsReceiver = std::function<void(const std::vector<slice>& ids, const C4Error* C4NULLABLE error)>;

    /// Requests a list of all document IDs, or optionally only those matching a pattern.
    /// The docIDs themselves are passed to a callback.
    /// The callback will be called zero or more times with a non-empty vector of docIDs,
    /// then once with an empty vector and an optional error.
    /// @param collectionID  The ID of the collection to observe.
    /// @param globPattern  Either `nullslice` or a glob-style pattern string (with `*` or
    ///                     `?` wildcards) for docIDs to match.
    /// @param callback  The callback to receive the docIDs.
    virtual void getAllDocIDs(slice collectionID, slice globPattern, AllDocsReceiver callback) = 0;

    /// Callback for \ref query.
    /// @param rowJSON  A row of the result, encoded as a JSON object.
    ///             On the final call, will be `nullslice`.
    /// @param rowDict  The row parsed as a Fleece Dict, if you requested it.
    /// @param error  NULL or, on the final call, a pointer to an error.
    using QueryReceiver = std::function<void(FLSlice rowJSON, FLDict rowDict, const C4Error* C4NULLABLE error)>;

    /// Runs a query on the server and gets the results.
    /// The callback will be called one or more times; see its documentation for details.
    /// @param name  The name by which the query has been registered on the server;
    ///              or a full query string beginning with "SELECT " or "{".
    /// @param parameters  A Dict mapping query parameter names to values.
    /// @param rowsAsFleece  True if you want the rows to be Fleece-encoded, false for JSON.
    /// @param receiver  A callback that will be invoked for each row of the result,
    ///                  and/or if there's an error.
    virtual void query(slice name, FLDict C4NULLABLE parameters, bool rowsAsFleece, QueryReceiver receiver) = 0;
#endif
};

C4_ASSUME_NONNULL_END
