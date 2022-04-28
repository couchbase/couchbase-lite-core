//
// ConnectedClient.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include "Async.hh"
#include "BLIPConnection.hh"
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
        bool deleted;
    };


    /** A callback invoked when one or more document IDs are received from a getAllDocIDs call.
        @param ids  A vector of document IDs. An empty vector indicates the result is complete.
        @param err  Points to the error, if any, else NULL. */
    using AllDocsReceiver = std::function<void(const std::vector<slice>& ids, const C4Error *err)>;


    /** A callback invoked when one or more documents change on the server. */
    using CollectionObserver = std::function<void(std::vector<C4CollectionObserver::Change> const&)>;


    /** A callback invoked for every row of a query result.
        @param result  An array of column values, or NULL if the query is complete or failed.
        @param error  Points to the error, else NULL. */
    using QueryReceiver = std::function<void(fleece::Array result, const C4Error* error)>;

    /** A live connection to Sync Gateway (or a CBL peer) that can do interactive CRUD operations.
        No C4Database necessary!
        Its API is somewhat similar to `Replicator`. */
    class ConnectedClient : public repl::Worker,
                            private blip::ConnectionDelegate
    {
    public:
        class Delegate;
        using CloseStatus   = blip::Connection::CloseStatus;
        using ActivityLevel = C4ReplicatorActivityLevel;
        using Status        = C4ReplicatorStatus;

        ConnectedClient(websocket::WebSocket* NONNULL,
                        Delegate&,
                        const C4ConnectedClientParameters&);

        /** ConnectedClient delegate API. (Similar to `Replicator::Delegate`) */
        class Delegate {
        public:
            virtual void clientGotHTTPResponse(ConnectedClient* NONNULL,
                                               int status,
                                               const websocket::Headers &headers) { }
            virtual void clientGotTLSCertificate(ConnectedClient* NONNULL,
                                                 slice certData) =0;
            virtual void clientStatusChanged(ConnectedClient* NONNULL,
                                             ActivityLevel) =0;
            virtual void clientConnectionClosed(ConnectedClient* NONNULL,
                                                const CloseStatus&)  { }

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
            virtual alloc_slice getBlobContents(const C4BlobKey &blobKey, C4Error *error);

            virtual ~Delegate() =default;
        };

        void start();
        
        void stop();
        
        void terminate();

        actor::Async<Status> status();

        //---- CRUD!

        /// Gets the current revision of a document from the server.
        /// You can set the `unlessRevID` parameter to avoid getting a redundant copy of a
        /// revision you already have.
        /// @param docID  The document ID.
        /// @param collectionID  The name of the document's collection, or `nullslice` for default.
        /// @param unlessRevID  If non-null, and equal to the current server-side revision ID,
        ///                     the server will return error {WebSocketDomain, 304}.
        /// @param asFleece  If true, the response's `body` field is Fleece; if false, it's JSON.
        /// @return  An async value that, when resolved, contains either a `DocResponse` struct
        ///          or a C4Error.
        actor::Async<DocResponse> getDoc(slice docID,
                                         slice collectionID,
                                         slice unlessRevID,
                                         bool asFleece = true);

        /// Downloads the contents of a blob given its digest.
        /// @param blobKey  The binary digest of the blob.
        /// @param compress  If true, a request that the server compress the blob's data during
        ///                  transmission. (This does not affect the data you receive.)
        /// @return  An async value that, when resolved, contains either the blob body or a C4Error.
        actor::Async<alloc_slice> getBlob(C4BlobKey blobKey,
                                          bool compress);

        /// Pushes a new document revision to the server.
        /// @note  If the document body contains any blob references, your delegate must implement
        ///        the \ref getBlobContents method.
        ///
        /// @param docID  The document ID.
        /// @param collectionID  The name of the document's collection, or `nullslice` for default.
        /// @param revID  The revision ID you're sending.
        /// @param parentRevID  The ID of the parent revision on the server,
        ///                     or `nullslice` if this is a new document.
        /// @param revisionFlags  Flags of this revision.
        /// @param fleeceData  The document body encoded as Fleece (without shared keys!)
        /// @return  An async value that, when resolved, contains the status as a C4Error.
        actor::Async<void> putDoc(slice docID,
                                  slice collectionID,
                                  slice revID,
                                  slice parentRevID,
                                  C4RevisionFlags revisionFlags,
                                  slice fleeceData);

        //---- All Documents

        /// Requests a list of all document IDs, optionally only those matching a pattern.
        /// The async return value resolves once a response is received from the server.
        /// The docIDs themselves are passed to a callback.
        /// The callback will be called zero or more times with a non-empty vector of docIDs,
        /// then once with an empty vector and an optional error.
        /// @param collectionID  The ID of the collection to observe.
        /// @param globPattern  Either `nullslice` or a glob-style pattern string (with `*` or
        ///                     `?` wildcards) for docIDs to match.
        /// @param callback  The callback to receive the docIDs.
        void getAllDocIDs(slice collectionID,
                          slice globPattern,
                          AllDocsReceiver callback);

        //---- Observer

        /// Registers a listener function that will be called when any document is changed.
        /// @note  To cancel, pass a null callback.
        /// @param collectionID  The ID of the collection to observe.
        /// @param callback  The function to call (on an arbitrary background thread!)
        /// @return  An async value that, when resolved, contains the status as a C4Error.
        actor::Async<void> observeCollection(slice collectionID,
                                             CollectionObserver callback);

        //---- Query

        /// Runs a query on the server and gets the results.
        /// @param name  The name by which the query has been registered on the server.
        /// @param parameters  A Dict mapping query parameter names to values.
        /// @param receiver  A callback that will be invoked for each row of the result,
        ///                  and/or if there's an error.
        void query(slice name,
                   fleece::Dict parameters,
                   QueryReceiver receiver);

        // exposed for unit tests:
        websocket::WebSocket* webSocket() const {return connection().webSocket();}
        
    protected:
        std::string loggingClassName() const override       {return "Client";}
        ActivityLevel computeActivityLevel() const override;
        void onHTTPResponse(int status, const websocket::Headers &headers) override;
        void onTLSCertificate(slice certData) override;
        void onConnect() override;
        void onClose(blip::Connection::CloseStatus status, blip::Connection::State state) override;
        void onRequestReceived(blip::MessageIn* request) override;

        void handleChanges(Retained<blip::MessageIn>);
        void handleGetAttachment(Retained<blip::MessageIn>);

    private:
        void setStatus(ActivityLevel);
        C4Error responseError(blip::MessageIn *response);
        void _disconnect(websocket::CloseCode closeCode, slice message);
        bool validateDocAndRevID(slice docID, slice revID);
        alloc_slice processIncomingDoc(slice docID, alloc_slice body, bool asFleece);
        void processOutgoingDoc(slice docID, slice revID, slice fleeceData, fleece::JSONEncoder &enc);
        bool receiveAllDocs(blip::MessageIn *, const AllDocsReceiver &);
        bool receiveQueryRows(blip::MessageIn*, const QueryReceiver&);
        bool receiveMultiLineQueryRows(blip::MessageIn*, const QueryReceiver&);

        Delegate*                   _delegate;         // Delegate whom I report progress/errors to
        C4ConnectedClientParameters _params;
        ActivityLevel               _activityLevel;
        Retained<ConnectedClient>   _selfRetain;
        CollectionObserver          _observer;
        mutable std::mutex          _mutex;
        bool                        _observing = false;
        bool                        _registeredChangesHandler = false;
        bool                        _remoteUsesVersionVectors = false;
        bool                        _remoteNeedsLegacyAttachments = true;
    };

}
