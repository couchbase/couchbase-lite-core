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
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN

struct C4ConnectedClient  : public fleece::RefCounted,
                            public fleece::InstanceCountedIn<C4Database>,
                            C4Base
{
    /// Creates a new ConnectedClient
    /// \note It will automatically starts the client, no need to call `start()`.
    ///
    /// @param params  Connected Client parameters.
    /// @result A new \ref C4ConnectedClient, or NULL on failure.
    static Retained<C4ConnectedClient> newClient(const C4ConnectedClientParameters &params);

    virtual litecore::actor::Async<C4ConnectedClientStatus> getStatus() const =0;

    /** Result of a successful `getDoc()` call. */
    struct DocResponse {
        alloc_slice docID, revID, body;
        bool deleted;
    };

    /// Gets the current revision of a document from the server.
    /// You can set the `unlessRevID` parameter to avoid getting a redundant copy of a
    /// revision you already have.
    /// @param docID  The document ID.
    /// @param collectionID  The name of the document's collection, or `nullslice` for default.
    /// @param unlessRevID  If non-null, and equal to the current server-side revision ID,
    ///                   the server will return error {WebSocketDomain, 304}.
    /// @param asFleece  If true, the response's `body` field is Fleece; if false, it's JSON.
    /// @result An async value that, when resolved, contains either a `DocResponse` struct
    ///          or a C4Error.
    virtual litecore::actor::Async<DocResponse> getDoc(slice docID,
                                                       slice collectionID,
                                                       slice unlessRevID,
                                                       bool asFleece)=0;
    
    /// Pushes a new document revision to the server.
    /// @param docID  The document ID.
    /// @param collectionID  The name of the document's collection, or `nullslice` for default.
    /// @param parentRevID The ID of the parent revision on the server,
    ///                      or `nullslice` if this is a new document.
    /// @param revisionFlags  Flags of this revision.
    /// @param fleeceData  The document body encoded as Fleece (without shared keys!)
    /// @return An async value that, when resolved, contains new revisionID or the status as a C4Error
    virtual litecore::actor::Async<std::string> putDoc(slice docID,
                                                       slice collectionID,
                                                       slice parentRevID,
                                                       C4RevisionFlags revisionFlags,
                                                       slice fleeceData)=0;

    /// Callback for \ref getAllDocIDs.
    /// @param ids  A vector of docIDs; empty on the final call.
    /// @param error  NULL or a pointer to an error.
    using AllDocsReceiver = std::function<void(const std::vector<slice>& ids,
                                               const C4Error* C4NULLABLE error)>;

    /// Requests a list of all document IDs, or optionally only those matching a pattern.
    /// The docIDs themselves are passed to a callback.
    /// The callback will be called zero or more times with a non-empty vector of docIDs,
    /// then once with an empty vector and an optional error.
    /// @param collectionID  The ID of the collection to observe.
    /// @param globPattern  Either `nullslice` or a glob-style pattern string (with `*` or
    ///                     `?` wildcards) for docIDs to match.
    /// @param callback  The callback to receive the docIDs.
    virtual void getAllDocIDs(slice collectionID,
                              slice globPattern,
                              AllDocsReceiver callback) =0;

    /// Callback for \ref query.
    /// @param row  On the first call, an array of colum names.
    ///             On subsequent calls, a result row as an array of column values.
    ///             On the final call, `nullptr`.
    /// @param error  NULL or, on the final call, a pointer to an error.
    using QueryReceiver = std::function<void(FLArray C4NULLABLE row,
                                             const C4Error* C4NULLABLE error)>;

    /// Runs a query on the server and gets the results.
    /// The callback will be called one or more times; see its documentation for details.
    /// @param name  The name by which the query has been registered on the server;
    ///              or a full query string beginning with "SELECT " or "{".
    /// @param parameters  A Dict mapping query parameter names to values.
    /// @param receiver  A callback that will be invoked for each row of the result,
    ///                  and/or if there's an error.
    virtual void query(slice name,
                       FLDict C4NULLABLE parameters,
                       QueryReceiver receiver) =0;

    /// Tells a connected client to start.
    virtual void start()=0;

    /// Tells a replicator to stop.
    virtual void stop()=0;
};

C4_ASSUME_NONNULL_END
