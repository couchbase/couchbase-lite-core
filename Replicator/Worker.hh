//
// Worker.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Actor.hh"
#include "ReplicatorOptions.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "Error.hh"
#include "ReplicatorTypes.hh"
#include "fleece/Fleece.hh"
#include <atomic>
#include <functional>
#include <memory>


namespace litecore { namespace repl {
    using fleece::RetainedConst;

    class DBAccess;
    class Replicator;
    class ReplicatedRev;

    extern LogDomain SyncBusyLog;

    /** Abstract base class of Actors used by the replicator, including `Replicator` itself.
        It provides:
        - Access to the replicator options, the database, and the BLIP connection.
        - A tree structure, via a `_parent` reference. Parents aggregate progress of children.
        - Progress, status, and error tracking. Changes are detected at the end of every Actor
          event and propagated to the parent, which aggregates them together with its own.
        - Some BLIP convenience methods for registering handlers and sending messages. */
    class Worker : public actor::Actor, public fleece::InstanceCountedIn<Worker> {
    public:
        
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        using ActivityLevel = C4ReplicatorActivityLevel;

        /** A key to set the collection that a worker is sending BLIP messages for

            Omitted if the default collection is being used, otherwise an index into
            the original list of collections received via getCollections.
        */
        static constexpr const char* kCollectionProperty = "collection";


        struct Status : public C4ReplicatorStatus {
            Status(ActivityLevel lvl =kC4Stopped) {
                level = lvl; error = {}; progress = progressDelta = {};
            }
            C4Progress progressDelta;
        };

        /// The Replicator at the top of the tree.
        /// Returns NULL if this Worker has stopped and discarded its parent link.
        /// Otherwise, the Replicator will remain alive at least until the Retained value returned
        /// by this method exits scope.
        virtual Retained<Replicator> replicatorIfAny();

        /// The Replicator at the top of the tree. Never NULL.
        /// @warning Throws rather than returning NULL.
        Retained<Replicator> replicator();

        /// True if the replicator is passive (run by the listener.)
        virtual bool passive() const {return false;}

        /// Called by the Replicator on its direct children when the BLIP connection closes.
        void connectionClosed() {
            enqueue(FUNCTION_TO_QUEUE(Worker::_connectionClosed));
        }

        /// Child workers call this on their parent when their status changes.
        void childChangedStatus(Worker *task, const Status &status) {
            enqueue(FUNCTION_TO_QUEUE(Worker::_childChangedStatus), Retained<Worker>(task), status);
        }

        C4ReplicatorProgressLevel progressNotificationLevel() const {
            return _options->progressLevel;
        }

        CollectionIndex collectionIndex() const         {return _collectionIndex;}

#if !DEBUG
    protected:
#endif
        /// True if there is a BLIP connection.
        bool connected() const                          {return _connection != nullptr;}

        /// The BLIP connection. Throws if there isn't one.
        blip::Connection& connection() const            {Assert(_connection); return *_connection;}

    protected:
        /// Designated constructor.
        /// @param connection  The BLIP connection.
        /// @param parent  The Worker that owns this one.
        /// @param options  The replicator options.
        /// @param db  Shared object providing thread-safe access to the C4Database.
        /// @param namePrefix  Prepended to the Actor name.
        Worker(blip::Connection *connection NONNULL,
               Worker *parent,
               const Options* options NONNULL,
               std::shared_ptr<DBAccess> db,
               const char *namePrefix NONNULL,
               CollectionIndex);

        /// Simplified constructor. Gets the other parameters from the parent object.
        Worker(Worker *parent NONNULL, const char *namePrefix NONNULL, CollectionIndex);

        virtual ~Worker();

        /// Override to specify an Actor mailbox that all children of this Worker should use.
        /// On Apple platforms, a mailbox is a GCD queue, so this reduces the number of queues.
        virtual actor::Mailbox* mailboxForChildren() {
            return _parent ? _parent->mailboxForChildren() : nullptr;
        }

        // overrides:
        virtual std::string loggingClassName() const override;
        virtual std::string loggingIdentifier() const override {return _loggingID;}
        virtual void afterEvent() override;
        virtual void caughtException(const std::exception &x) override;

        fleece::Retained<C4Collection> collection();

#pragma mark - BLIP:

        /// True if the WebSocket connection is open and acting as a client (active).
        bool isOpenClient() const               {return _connection &&
                                                 _connection->role() == websocket::Role::Client;}
        /// True if the WebSocket connection is open and acting as a server (passive).
        bool isOpenServer() const               {return _connection &&
                                                 _connection->role() == websocket::Role::Server;}
        /// True if the replicator is continuous.
        bool isContinuous() const               {
            auto collIndex = collectionIndex();
            if (collIndex == kNotCollectionIndex) {
                //TBD: this is a Replicator. what should it be?
                collIndex = 0;
            }
            return _options->pushOf(collIndex) == kC4Continuous
                || _options->pullOf(collIndex) == kC4Continuous;
        }

        /// Implementation of public `connectionClosed`. May be overridden, but call super.
        virtual void _connectionClosed() {
            logDebug("connectionClosed");
            _connection = nullptr;
        }

        /// Registers a method to run when a BLIP request with the given profile arrives.
        template <class ACTOR>
        void registerHandler(const char *profile NONNULL,
                             void (ACTOR::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(
                                        std::bind(method, (ACTOR*)this, std::placeholders::_1) );
            _connection->setRequestHandler(profile, false, asynchronize(profile, fn));
        }

        /// Sends a BLIP request. Increments `_pendingResponseCount` until the response is
        /// complete, keeping this Worker in the busy state.
        void sendRequest(blip::MessageBuilder& builder,
                         blip::MessageProgressCallback onProgress = nullptr);

        /// The number of BLIP responses I'm waiting for.
        int pendingResponseCount() const        {return _pendingResponseCount;}

#pragma mark - ERRORS:

        /// Logs the message's error and calls `onError`.
        void gotError(const blip::MessageIn* NONNULL);
        /// Logs a fatal error and calls `onError`.
        void gotError(C4Error);

        /// Sets my status's `error` property. Call `gotError` instead, but you can override.
        virtual void onError(C4Error);

        /// Reports a less-serious error that affects a document but doesn't stop replication.
        virtual void finishedDocumentWithError(ReplicatedRev* NONNULL, C4Error, bool transientErr);

        /// Reports that a document has been completed.
        void finishedDocument(ReplicatedRev*);

        /// Static utility to convert LiteCore --> BLIP errors.
        static blip::ErrorBuf c4ToBLIPError(C4Error);
        /// Static utility to convert BLIP --> LiteCore errors.
        static C4Error blipToC4Error(const blip::Error&);

#pragma mark - STATUS & PROGRESS:

        /// My current status.
        const Status& status() const            {return _status;}

        /// Called by `afterEvent` if my status has changed.
        /// Default implementation calls the parent's `childChangedStatus`,
        /// then if status is `kC4Stopped`, clears the parent pointer.
        virtual void changedStatus();

        /// Implementation of public `childChangedStatus`; called on this Actor's thread.
        /// Does nothing, but you can override.
        virtual void _childChangedStatus(Retained<Worker> task, Status) { 
        }

        /// Adds the counts in the given struct to my status's progress.
        void addProgress(C4Progress);
        /// Directly sets my status's progress counts.
        void setProgress(C4Progress);

        /// Determines whether I'm stopped/idle/busy.
        /// Called after every event, to update `_status.level`.
        /// The default implementation returns `kC4Busy` if there are pending BLIP responses,
        /// or this Actor has pending events in its queue, else `kC4Idle`.
        virtual ActivityLevel computeActivityLevel() const;

#pragma mark - INSTANCE DATA:
    protected:
        RetainedConst<Options>      _options;                   // The replicator options
        Retained<Worker>            _parent;                    // Worker that owns me
        std::shared_ptr<DBAccess>   _db;                        // Database
        std::string                 _loggingID;                 // My name in the log
        uint8_t                     _importance {1};            // Higher values log more
    private:
        Retained<blip::Connection>  _connection;                // BLIP connection
        int                         _pendingResponseCount {0};  // # of responses I'm awaiting
        Status                      _status {kC4Idle};          // My status
        bool                        _statusChanged {false};     // Status changed during this event
        const CollectionIndex       _collectionIndex;
    };

} }
