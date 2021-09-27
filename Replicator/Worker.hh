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
#include "fleece/Fleece.hh"
#include <atomic>
#include <functional>
#include <memory>


namespace litecore { namespace repl {
    class DBAccess;
    class Replicator;
    class ReplicatedRev;

    extern LogDomain SyncBusyLog;

    /** Abstract base class of Actors used by the replicator */
    class Worker : public actor::Actor, public fleece::InstanceCountedIn<Worker> {
    public:
        
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        using ActivityLevel = C4ReplicatorActivityLevel;


        struct Status : public C4ReplicatorStatus {
            Status(ActivityLevel lvl =kC4Stopped) {
                level = lvl; error = {}; progress = progressDelta = {};
            }
            C4Progress progressDelta;
        };

        virtual Retained<Replicator> replicatorIfAny();     // may return null
        Retained<Replicator> replicator();                  // throws rather than return null

        bool passive() const                                {return _passive;}

        /** Called by the Replicator when the BLIP connection closes. */
        void connectionClosed() {
            enqueue(FUNCTION_TO_QUEUE(Worker::_connectionClosed));
        }

        /** Called by child actors when their status changes. */
        void childChangedStatus(Worker *task, const Status &status) {
            enqueue(FUNCTION_TO_QUEUE(Worker::_childChangedStatus), task, status);
        }

        // Tech Debt: Where is the line between worker and replicator?
        // Also, this level should be an enum
        virtual int progressNotificationLevel() const   {return _progressNotificationLevel;}
        void setProgressNotificationLevel(int level);

#if !DEBUG
    protected:
#endif
        bool connected() const                          {return _connection != nullptr;}
        blip::Connection& connection() const            {Assert(_connection); return *_connection;}

    protected:
        Worker(blip::Connection *connection NONNULL,
               Worker *parent,
               const Options &options,
               std::shared_ptr<DBAccess>,
               const char *namePrefix NONNULL);

        Worker(Worker *parent NONNULL, const char *namePrefix NONNULL);

        ~Worker();

        virtual std::string loggingClassName() const override;

        virtual actor::Mailbox* mailboxForChildren() {
            return _parent ? _parent->mailboxForChildren() : nullptr;
        }

        /** Registers a callback to run when a BLIP request with the given profile arrives. */
        template <class ACTOR>
        void registerHandler(const char *profile NONNULL,
                             void (ACTOR::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(
                                        std::bind(method, (ACTOR*)this, std::placeholders::_1) );
            _connection->setRequestHandler(profile, false, asynchronize(profile, fn));
        }

        /** Implementation of connectionClosed(). May be overridden, but call super. */
        virtual void _connectionClosed() {
            logDebug("connectionClosed");
            _connection = nullptr;
        }

        /** Convenience to send a BLIP request. */
        void sendRequest(blip::MessageBuilder& builder,
                         blip::MessageProgressCallback onProgress = nullptr);

        void gotError(const blip::MessageIn* NONNULL);
        void gotError(C4Error) ;
        virtual void onError(C4Error);         // don't call this, but you can override

        /** Report less-serious errors that affect a document but don't stop replication. */
        virtual void finishedDocumentWithError(ReplicatedRev* NONNULL, C4Error, bool transientErr);

        void finishedDocument(ReplicatedRev*);

        static blip::ErrorBuf c4ToBLIPError(C4Error);
        static C4Error blipToC4Error(const blip::Error&);

        static inline bool isNotFoundError(C4Error err) {
            return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
        }

        bool isOpenClient() const               {return _connection && _connection->role() == websocket::Role::Client;}
        bool isOpenServer() const               {return _connection && _connection->role() == websocket::Role::Server;}
        bool isContinuous() const               {return _options.push == kC4Continuous
                                                     || _options.pull == kC4Continuous;}
        const Status& status() const            {return _status;}
        virtual ActivityLevel computeActivityLevel() const;
        virtual void changedStatus();
        void addProgress(C4Progress);
        void setProgress(C4Progress);

        virtual void _childChangedStatus(Worker *task, Status) { }

        virtual void afterEvent() override;
        virtual void caughtException(const std::exception &x) override;

        virtual std::string loggingIdentifier() const override {return _loggingID;}
        int pendingResponseCount() const        {return _pendingResponseCount;}

        Options _options;
        Retained<Worker> _parent;
        std::shared_ptr<DBAccess> _db;
        uint8_t _important {1};
        bool _passive {false};
        std::string _loggingID;

    private:
        Retained<blip::Connection> _connection;
        int _pendingResponseCount {0};
        std::atomic_int _progressNotificationLevel;
        Status _status {kC4Idle};
        bool _statusChanged {false};
    };

} }
