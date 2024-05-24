//
// ConnectedClient.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include "BLIPConnection.hh"
#include "Result.hh"
#include "c4ConnectedClientTypes.h"
#include "c4Observer.hh"
#include "c4ReplicatorTypes.h"
#include "fleece/Fleece.hh"
#include <functional>
#include <variant>
#include <vector>

namespace litecore::client {

    /** Result of a successful `ConnectedClient::getDoc()` call. */
    struct DocResponse {
        alloc_slice docID, revID, body;
        bool        deleted;
    };

    /** A callback invoked when one or more document IDs are received from a getAllDocIDs call.
        @param ids  A vector of document IDs. An empty vector indicates the result is complete.
        @param err  Points to the error, if any, else NULL. */
    using AllDocsReceiver = std::function<void(const std::vector<slice>& ids, const C4Error* err)>;


    /** A callback invoked when one or more documents change on the server. */
    using CollectionObserver =
            std::function<void(std::vector<C4CollectionObserver::Change> const&, const C4Error* err)>;


    /** A callback invoked for every row of a query result.
        @param rowJSON  The row as a JSON-encoded object, or `nullslice` on the final call.
        @param rowDict  The row as a Fleece `Dict` object, if you requested it, or `nullptr`.
        @param error  Points to the error, else NULL. */
    using QueryReceiver = std::function<void(slice rowJSON, fleece::Dict rowDict, const C4Error* error)>;

    /** A live connection to Sync Gateway (or a CBL peer) that can do interactive CRUD operations.
        No C4Database necessary!
        Its API is somewhat similar to `Replicator`. */
    class ConnectedClient
        : public repl::Worker
        , public blip::ConnectionDelegate {
      public:
        class Delegate;
        using CloseStatus   = blip::Connection::CloseStatus;
        using ActivityLevel = C4ReplicatorActivityLevel;
        using Status        = C4ReplicatorStatus;

        ConnectedClient(websocket::WebSocket* NONNULL, Delegate&, const C4ConnectedClientParameters&,
                        repl::Options*        NONNULL);

        /** ConnectedClient delegate API. (Similar to `Replicator::Delegate`) */
        class Delegate {
          public:
            virtual void clientGotHTTPResponse(ConnectedClient* NONNULL, int status,
                                               const websocket::Headers& headers) {}

            virtual void clientGotTLSCertificate(ConnectedClient* NONNULL, slice certData) = 0;
            virtual void clientStatusChanged(ConnectedClient* NONNULL, const Status&)      = 0;

            virtual void clientConnectionClosed(ConnectedClient* NONNULL, const CloseStatus&) {}

            /** Returns the contents of a blob given its key (SHA-1 digest) as found in a blob in
                a document being uploaded to the server.

                This method is called after the \ref putDoc method is called, but before its async
                value resolves. It's not guaranteed to be called for every blob in the document,
                only those that are not yet known to the server.

                You must override this method if you upload documents containing blobs.
                The default implementation always returns a Not Found error,
                which will cause the upload to fail.
                @param blobKey  The blob's binary digest.
                @param error  If you can't return the contents, store an error here.
                @return  The blob's contents, or `nullslice` if an error occurred. */
            virtual alloc_slice getBlobContents(const C4BlobKey& blobKey, C4Error* error);

            virtual ~Delegate() = default;
        };

        void start();

        void stop();

        void terminate();

        Status status();

        //---- CRUD!

        /// Gets the current revision of a document from the server.
        /// You can set the `unlessRevID` parameter to avoid getting a redundant copy of a
        /// revision you already have.
        /// @param docID  The document ID.
        /// @param collection  The document's collection.
        /// @param unlessRevID  If non-null, and equal to the current server-side revision ID,
        ///                     the server will return error {WebSocketDomain, 304}.
        /// @param asFleece  If true, the response's `body` field is Fleece; if false, it's JSON.
        /// @param callback  On completion will be called with either a `DocResponse` struct
        ///          or a C4Error.
        void getDoc(C4CollectionSpec const& collection, slice docID, slice unlessRevID, bool asFleece,
                    std::function<void(Result<DocResponse>)> callback);

        /// Downloads the contents of a blob given its digest.
        /// @param collection  The collection containing the blob.
        /// @param blobKey  The binary digest of the blob.
        /// @param compress  If true, a request that the server compress the blob's data during
        ///                  transmission. (This does not affect the data you receive.)
        /// @param callback  On completion will be called with either the blob body or a C4Error.
        void getBlob(C4CollectionSpec const& collection, C4BlobKey blobKey, bool compress,
                     std::function<void(Result<alloc_slice>)> callback);

        /// Pushes a new document revision to the server.
        /// @note  If the document body contains any blob references, your delegate must implement
        ///        the \ref getBlobContents method.
        ///
        /// @param docID  The document ID.
        /// @param collection  The document's collection.
        /// @param revID  The revision ID you're sending.
        /// @param parentRevID  The ID of the parent revision on the server,
        ///                     or `nullslice` if this is a new document.
        /// @param revisionFlags  Flags of this revision.
        /// @param fleeceData  The document body encoded as Fleece (without shared keys!)
        /// @param callback  On completion will be called with the status as a C4Error.
        void putDoc(C4CollectionSpec const& collection, slice docID, slice revID, slice parentRevID,
                    C4RevisionFlags revisionFlags, slice fleeceData, std::function<void(Result<void>)> callback);

        //---- All Documents

        /// Requests a list of all document IDs, or optionally only those matching a pattern.
        /// The docIDs themselves are passed to a callback.
        /// The callback will be called zero or more times with a non-empty vector of docIDs,
        /// then once with an empty vector and an optional error.
        /// @param collection  The collection to observe.
        /// @param globPattern  Either `nullslice` or a glob-style pattern string (with `*` or
        ///                     `?` wildcards) for docIDs to match.
        /// @param callback  The callback to receive the docIDs.
        void getAllDocIDs(C4CollectionSpec const& collection, slice globPattern, AllDocsReceiver callback);

        //---- Observer

        /// Registers a listener function that will be called when any document is changed.
        /// @note  To cancel, pass a null callback.
        /// @param collection  The collection to observe.
        /// @param callback  The function to call (on an arbitrary background thread!)
        void observeCollection(C4CollectionSpec const& collection, CollectionObserver callback);

        //---- Query

        /// Runs a query on the server and gets the results.
        /// @param name  The name by which the query has been registered on the server;
        ///              or a full query string beginning with "SELECT " or "{".
        /// @param parameters  A Dict mapping query parameter names to values.
        /// @param asFleece If true, rows will be parsed as Fleece dicts for the callback.
        /// @param receiver  A callback that will be invoked for each row of the result,
        ///                  and/or if there's an error.
        void query(slice name, fleece::Dict parameters, bool asFleece, QueryReceiver receiver);

        // exposed for unit tests:
        websocket::WebSocket* webSocket() const { return connection().webSocket(); }

      protected:
        std::string loggingClassName() const override { return "Client"; }

        ActivityLevel computeActivityLevel() const override;
        void          onHTTPResponse(int status, const websocket::Headers& headers) override;
        void          onTLSCertificate(slice certData) override;
        void          onConnect() override;
        void          onClose(blip::Connection::CloseStatus status, blip::Connection::State state) override;
        void          onRequestReceived(blip::MessageIn* request) override;

        void handleChanges(Retained<blip::MessageIn>);
        void handleGetAttachment(Retained<blip::MessageIn>);

      private:
        enum class CollectionIndex : unsigned {};

        void _start();
        void _stop();
        void _onHTTPResponse(int status, websocket::Headers headers);
        void _onConnect();
        void _onClose(blip::Connection::CloseStatus status, blip::Connection::State state);
        void _onRequestReceived(Retained<blip::MessageIn> request);
        void _observeCollection(CollectionIndex, CollectionObserver callback);

        void            setStatus(ActivityLevel);
        void            assertConnected();
        CollectionIndex getCollectionID(C4CollectionSpec const&) const;
        void            addCollectionProperty(blip::MessageBuilder&, C4CollectionSpec const&) const;
        C4Error         responseError(blip::MessageIn* response);
        void            _disconnect(websocket::CloseCode closeCode, slice message);
        bool            validateDocAndRevID(slice docID, slice revID);
        alloc_slice     processIncomingDoc(slice docID, alloc_slice body, bool asFleece);
        void            processOutgoingDoc(slice docID, slice revID, slice fleeceData, fleece::JSONEncoder& enc);
        bool            receiveAllDocs(blip::MessageIn*, const AllDocsReceiver&);
        bool            receiveQueryRows(blip::MessageIn*, const QueryReceiver&, bool asFleece);

        Retained<WeakHolder<blip::ConnectionDelegate>> _weakConnectionDelegateThis;
        Delegate*                                      _delegate;  // Delegate whom I report progress/errors to
        C4ConnectedClientParameters                    _params;
        std::vector<std::string>                       _collections;
        ActivityLevel                                  _activityLevel;
        Retained<ConnectedClient>                      _selfRetain;
        CollectionObserver                             _observer;
        mutable std::mutex                             _mutex;
        bool                                           _observing                    = false;
        bool                                           _registeredChangesHandler     = false;
        bool                                           _remoteUsesVersionVectors     = false;
        bool                                           _remoteNeedsLegacyAttachments = true;
    };

}  // namespace litecore::client
