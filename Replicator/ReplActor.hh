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
    class ReplActor : public Actor {

    protected:

        blip::Connection* connection() const                {return _connection;}
        virtual void setConnection(blip::Connection *connection);

        template <class ACTOR>
        void registerHandler(const char *profile,
                             void (ACTOR::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(
                                        std::bind(method, (ACTOR*)this, std::placeholders::_1) );
            _connection->setRequestHandler(profile, asynchronize(fn));
        }

        blip::FutureResponse sendRequest(blip::MessageBuilder& builder) {
            return connection()->sendRequest(builder);
        }

        void gotError(const blip::MessageIn*);
        void gotError(C4Error);

    private:
        blip::Connection* _connection {nullptr};
    };

} }


#define SPLAT(S)    (int)(S).size, (S).buf      // Use with %.* formatter
