//
// Worker.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "ReplicatorOptions.hh"
#include "DBAccess.hh"
#include "Actor.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "Increment.hh"
#include "Timer.hh"
#include "c4.hh"
#include "c4Private.h"
#include "fleece/Fleece.hh"
#include "Error.hh"
#include <functional>
#include <memory>


namespace litecore { namespace repl {
    class Replicator;
    class ReplicatedRev;

    extern LogDomain SyncBusyLog;

    /** Abstract base class of Actors used by the replicator */
    class Worker : public actor::Actor, fleece::InstanceCountedIn<Worker>, protected Logging {
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

        Replicator* replicator() const;

        /** Called by the Replicator when the BLIP connection closes. */
        void connectionClosed() {
            enqueue(&Worker::_connectionClosed);
        }

        /** Called by child actors when their status changes. */
        void childChangedStatus(Worker *task, const Status &status) {
            enqueue(&Worker::_childChangedStatus, task, status);
        }

#if !DEBUG
    protected:
#endif
        blip::Connection* connection() const                {return _connection;}

    protected:
        Worker(blip::Connection *connection NONNULL,
               Worker *parent,
               const Options &options,
               std::shared_ptr<DBAccess>,
               const char *namePrefix NONNULL);

        Worker(Worker *parent, const char *namePrefix NONNULL);

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
            _connection->setRequestHandler(profile, false, asynchronize(fn));
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

        int progressNotificationLevel() const   {return _progressNotificationLevel;}

        virtual void _childChangedStatus(Worker *task, Status) { }

        virtual void afterEvent() override;
        virtual void caughtException(const std::exception &x) override;

        virtual std::string loggingIdentifier() const override {return _loggingID;}
        int pendingResponseCount() const        {return _pendingResponseCount;}

        Options _options;
        Retained<Worker> _parent;
        std::shared_ptr<DBAccess> _db;
        uint8_t _important {1};
        std::string _loggingID;

    private:
        Retained<blip::Connection> _connection;
        int _pendingResponseCount {0};
        int _progressNotificationLevel;
        Status _status {kC4Idle};
        bool _statusChanged {false};
    };

} }
