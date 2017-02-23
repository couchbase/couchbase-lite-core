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

    extern LogDomain SyncLog;


    /** Abstract base class of Actors used by the replicator */
    class ReplActor : public Actor, InstanceCounted, protected Logging {
    public:
        struct Options {
            bool push;
            bool pull;
            bool continuous;
        };

        void connectionClosed() {
            enqueue(&ReplActor::_connectionClosed);
        }

        const std::string& name() const                     {return _name;}

#if !DEBUG
    protected:
#endif
        blip::Connection* connection() const                {return _connection;}

    protected:
        ReplActor(blip::Connection *connection, Options options, const std::string &name)
        :Logging(SyncLog)
        ,_connection(connection)
        ,_options(options)
        ,_name(name)
        { }

        template <class ACTOR>
        void registerHandler(const char *profile,
                             void (ACTOR::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(
                                        std::bind(method, (ACTOR*)this, std::placeholders::_1) );
            _connection->setRequestHandler(profile, asynchronize(fn));
        }

        virtual void _connectionClosed() {
            _connection = nullptr;
        }

        blip::FutureResponse sendRequest(blip::MessageBuilder& builder) {
            return connection()->sendRequest(builder);
        }

        void gotError(const blip::MessageIn*);
        void gotError(C4Error);

        virtual void afterEvent() override                  {setBusy(eventCount() > 1);}
        void setBusy(bool busy);

        virtual std::string loggingIdentifier() const override;

        const Options _options;

    private:
        Retained<blip::Connection> _connection;
        std::string _name;
        bool _busy {false};
    };

} }


#define SPLAT(S)    (int)(S).size, (S).buf      // Use with %.* formatter
