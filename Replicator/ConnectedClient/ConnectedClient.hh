//
// ConnectedClient.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include "Async.hh"
#include "BLIPConnection.hh"
#include "c4Observer.hh"
#include "c4ReplicatorTypes.h"
#include <functional>
#include <variant>
#include <vector>

namespace litecore::client {

    /** Result of a successful `ConnectedClient::getDoc()` call. */
    struct DocResponse {
        alloc_slice docID, revID, body;
        bool deleted;
    };


    /** A callback invoked when one or more documents change on the server. */
    using CollectionObserver = std::function<void(std::vector<C4CollectionObserver::Change> const&)>;


    /** A live connection to Sync Gateway (or a CBL peer) that can do interactive CRUD operations.
        No C4Database necessary!
        Its API is somewhat similar to `Replicator`. */
    class ConnectedClient : public repl::Worker,
                            private blip::ConnectionDelegate
    {
    public:
        class Delegate;
        using CloseStatus = blip::Connection::CloseStatus;
        using ActivityLevel = C4ReplicatorActivityLevel;

        ConnectedClient(websocket::WebSocket* NONNULL,
                        Delegate&,
                        fleece::AllocedDict options);

        /** ConnectedClient Delegate API. Almost identical to `Replicator::Delegate` */
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
            virtual ~Delegate() =default;
        };

        void start();
        
        void stop();
        
        void terminate();

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

        /// Gets the contents of a blob given its digest.
        /// @param blobKey  The binary digest of the blob.
        /// @param compress  True if the blob should be downloaded in compressed form.
        /// @return  An async value that, when resolved, contains either the blob body or a C4Error.
        actor::Async<alloc_slice> getBlob(C4BlobKey blobKey,
                                          bool compress);

        /// Pushes a new document revision to the server.
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

        /// Registers a listener function that will be called when any document is changed.
        /// @note  To cancel, pass a null callback.
        /// @param collectionID  The ID of the collection to observe.
        /// @param callback  The function to call (on an arbitrary background thread!)
        /// @return  An async value that, when resolved, contains the status as a C4Error.
        actor::Async<void> observeCollection(slice collectionID,
                                             CollectionObserver callback);

        // exposed for unit tests:
        websocket::WebSocket* webSocket() const {return connection().webSocket();}
        
    protected:
        std::string loggingClassName() const override       {return "Client";}
        void onHTTPResponse(int status, const websocket::Headers &headers) override;
        void onTLSCertificate(slice certData) override;
        void onConnect() override;
        void onClose(blip::Connection::CloseStatus status, blip::Connection::State state) override;
        void onRequestReceived(blip::MessageIn* request) override;

        void handleChanges(Retained<blip::MessageIn>);

    private:
        void setStatus(ActivityLevel);
        C4Error responseError(blip::MessageIn *response);
        void _disconnect(websocket::CloseCode closeCode, slice message);
        bool validateDocAndRevID(slice docID, slice revID);

        Delegate*                   _delegate;         // Delegate whom I report progress/errors to
        ActivityLevel               _status;
        Retained<ConnectedClient>   _selfRetain;
        CollectionObserver          _observer;
        bool                        _observing = false;
        bool                        _registeredChangesHandler = false;
        bool                        _remoteUsesVersionVectors = false;
        mutable std::mutex          _mutex;
    };

}
