//
//  Replicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Actor.hh"
#include "BLIPConnection.hh"
#include "Base.hh"
#include "c4Database.h"

using namespace litecore::blip;

namespace litecore { namespace repl {

    class Pusher;
    class Puller;


    class Replicator : public Actor, ConnectionDelegate {
    public:
        struct Options {
            bool push {false};
            bool pull {false};
            bool continuous {false};
        };

        Replicator(C4Database*, WebSocketProvider&, const WebSocketAddress&&, Options);

        FutureResponse sendRequest(MessageBuilder& builder) {
            return connection()->sendRequest(builder);
        }

    protected:
        virtual void onConnect() override               {enqueue(&Replicator::_onConnect);}
        virtual void onError(int errcode, fleece::slice reason) override
                                                        {enqueue(&Replicator::_onError, errcode, reason);}
        virtual void onClose(int status, fleece::slice reason) override
                                                        {enqueue(&Replicator::_onClose, status, reason);}
        virtual void onRequestReceived(blip::MessageIn *msg) override
                                                        {enqueue(&Replicator::_onRequestReceived, msg);}

    private:
        struct Checkpoint {
            sequence_t localSeq {0};
            std::string remoteSeq;
        };

        void _onConnect();
        void _onError(int errcode, fleece::slice reason);
        void _onClose(int status, fleece::slice reason);
        void _onRequestReceived(blip::MessageIn *msg);

        void gotError(const MessageIn* msg);
        void getCheckpoint();
        std::string effectiveRemoteCheckpointDocID();
        Checkpoint decodeCheckpoint(slice json);
        void startReplicating();

        C4Database *_db;
        const WebSocketAddress _remoteAddress;
        const Options _options;
        Retained<Connection> _connection;
        std::string _remoteCheckpointDocID;
        Checkpoint _checkpoint;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;
    };

} }
