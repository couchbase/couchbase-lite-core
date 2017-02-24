//
//  ReplActor.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Actor.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "c4.hh"
#include <functional>


namespace litecore { namespace repl {

    /** Abstract base class of Actors used by the replicator */
    class ReplActor : public Actor, InstanceCounted, protected Logging {
    public:
        struct Options {
            bool push;
            bool pull;
            bool continuous;
        };

        /** Called by the Replicator when the BLIP connection closes. */
        void connectionClosed() {
            enqueue(&ReplActor::_connectionClosed);
        }

#if !DEBUG
    protected:
#endif
        blip::Connection* connection() const                {return _connection;}

    protected:
        static LogDomain SyncLog;

        ReplActor(blip::Connection *connection, Options options, const std::string &loggingID)
        :Logging(SyncLog)
        ,_connection(connection)
        ,_options(options)
        ,_loggingIdentifier(loggingID)
        { }

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
        blip::FutureResponse sendRequest(blip::MessageBuilder& builder) {
            return _connection->sendRequest(builder);
        }

        void gotError(const blip::MessageIn*);
        void gotError(C4Error);

        virtual void afterEvent() override                  {setBusy(eventCount() > 1);}
        void setBusy(bool busy);

        virtual std::string loggingIdentifier() const override {
            return _loggingIdentifier;
        }

        Options _options;

    private:
        Retained<blip::Connection> _connection;
        std::string _loggingIdentifier;
        bool _busy {false};
    };

} }


#define SPLAT(S)    (int)(S).size, (S).buf      // Use with %.* formatter
