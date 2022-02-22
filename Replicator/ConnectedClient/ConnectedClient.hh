//
// ConnectedClient.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include "Async.hh"
#include "BLIPConnection.hh"
#include "c4ReplicatorTypes.h"
#include <variant>

namespace litecore::client {

    /** Result of a successful `ConnectedClient::getDoc()` call. */
    struct DocResponse {
        alloc_slice docID, revID, body;
        bool deleted;
    };

    /** Result type of `ConnectedClient::getDoc()` -- either a response or an error. */
    using DocResponseOrError = std::variant<DocResponse,C4Error>;

    /** Result type of `ConnectedClient::getBlob()` -- either blob contents or an error. */
    using BlobOrError = std::variant<alloc_slice,C4Error>;



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
        actor::Async<DocResponseOrError> getDoc(alloc_slice docID,
                                                alloc_slice collectionID,
                                                alloc_slice unlessRevID,
                                                bool asFleece = true);

        /// Gets the contents of a blob given its digest.
        /// @param blobKey  The binary digest of the blob.
        /// @param compress  True if the blob should be downloaded in compressed form.
        /// @return  An async value that, when resolved, contains either the blob body or a C4Error.
        actor::Async<BlobOrError> getBlob(C4BlobKey blobKey,
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
        actor::Async<C4Error> putDoc(alloc_slice docID,
                                     alloc_slice collectionID,
                                     alloc_slice revID,
                                     alloc_slice parentRevID,
                                     C4RevisionFlags revisionFlags,
                                     alloc_slice fleeceData);

        // exposed for unit tests:
        websocket::WebSocket* webSocket() const {return connection().webSocket();}
        
    protected:
        std::string loggingClassName() const override       {return "Client";}
        void onHTTPResponse(int status, const websocket::Headers &headers) override;
        void onTLSCertificate(slice certData) override;
        void onConnect() override;
        void onClose(blip::Connection::CloseStatus status, blip::Connection::State state) override;
        void onRequestReceived(blip::MessageIn* request) override;

    private:
        void setStatus(ActivityLevel);
        C4Error responseError(blip::MessageIn *response);
        void _disconnect(websocket::CloseCode closeCode, slice message);

        Delegate*               _delegate;             // Delegate whom I report progress/errors to
        ActivityLevel           _status;
        Retained<ConnectedClient> _selfRetain;
    };

}
