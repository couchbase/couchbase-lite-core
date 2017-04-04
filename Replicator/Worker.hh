//
//  Worker.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Actor.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "Timer.hh"
#include "c4.hh"
#include "c4Replicator.h"
#include "c4Private.h"
#include <chrono>
#include <functional>


namespace litecore { namespace repl {
    class Replicator;

    /** Time duration unit: seconds, stored as 64-bit floating point. */
    using duration = std::chrono::nanoseconds;


    /** Abstract base class of Actors used by the replicator */
    class Worker : public Actor, C4InstanceCounted, protected Logging {
    public:

        /** Replication configuration options */
        struct Options {
            using Mode = C4ReplicatorMode;

            Mode     push                   {kC4Disabled};
            Mode     pull                   {kC4Disabled};
            duration checkpointSaveDelay    {std::chrono::seconds(5)};

            Options()
            { }
            
            Options(Mode push_, Mode pull_)
            :push(push_), pull(pull_)
            { }

            static Options pushing(Mode mode =kC4OneShot)  {return Options(mode, kC4Disabled);}
            static Options pulling(Mode mode =kC4OneShot)  {return Options(kC4Disabled, mode);}
            static Options passive()                       {return Options(kC4Passive,kC4Passive);}
        };

        using ActivityLevel = C4ReplicatorActivityLevel;

        struct Status : public C4ReplicatorStatus {
            Status(ActivityLevel lvl =kC4Stopped) {
                level = lvl; error = {}; progress = progressDelta = {};
            }
            C4Progress progressDelta;
        };

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
        Worker(blip::Connection *connection,
                  Worker *parent,
                  Options options,
                  const char *namePrefix);

        Worker(Worker *parent, const char *namePrefix);

        ~Worker();

        /** Registers a callback to run when a BLIP request with the given profile arrives. */
        template <class ACTOR>
        void registerHandler(const char *profile,
                             void (ACTOR::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(
                                        std::bind(method, (ACTOR*)this, std::placeholders::_1) );
            _connection->setRequestHandler(profile, asynchronize(fn));
        }

        /** Implementation of connectionClosed(). Maybe overridden, but call super. */
        virtual void _connectionClosed() {
            _connection = nullptr;
        }

        /** Convenience to send a BLIP request. */
        void sendRequest(blip::MessageBuilder& builder,
                         blip::MessageProgressCallback onProgress = nullptr);

        void gotError(const blip::MessageIn*);
        void gotError(C4Error);

        static blip::ErrorBuf c4ToBLIPError(C4Error);
        static C4Error blipToC4Error(const blip::Error&);

        bool isOpenClient() const               {return _connection && !_connection->isServer();}
        bool isOpenServer() const               {return _connection &&  _connection->isServer();}
        bool isContinuous() const               {return _options.push == kC4Continuous
                                                     || _options.pull == kC4Continuous;}
        const Status& status() const            {return _status;}
        virtual ActivityLevel computeActivityLevel() const;
        virtual void changedStatus();
        void addProgress(C4Progress);
        void setProgress(C4Progress);

        virtual void _childChangedStatus(Worker *task, Status) { }

       virtual void afterEvent() override;

        virtual std::string loggingIdentifier() const override {
            return actorName();
        }

        Options _options;
        Worker* _parent;
        bool _important {true};

    private:
        Retained<blip::Connection> _connection;
        int _pendingResponseCount {0};
        Status _status { };
        bool _statusChanged {false};
    };

} }
