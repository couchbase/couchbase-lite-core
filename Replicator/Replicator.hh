//
//  Replicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "ReplActor.hh"
#include "Checkpoint.hh"
#include "BLIPConnection.hh"
#include "FleeceCpp.hh"

using namespace litecore::blip;

namespace litecore { namespace repl {

    class Pusher;
    class Puller;
    class DBActor;


    /** The top-level replicator object, which runs the BLIP connection.
        Pull and push operations are run by subidiary Puller and Pusher objects.
        The database will only be accessed by the DBAgent object. */
    class Replicator : public ReplActor, ConnectionDelegate {
    public:

        class Delegate;
        using CloseStatus = blip::Connection::CloseStatus;

        /** Constructor for a client connection; will open the Connection itself. */
        Replicator(C4Database*,
                   websocket::Provider&,
                   const websocket::Address&,
                   Delegate&,
                   Options);

        /** Constructor for an incoming (server) connection. */
        Replicator(C4Database*,
                   websocket::WebSocket*,
                   const websocket::Address&,
                   Delegate&,
                   Options = Options::passive());

        /** Replicator delegate; receives progress & error notifications. */
        class Delegate {
        public:
            virtual ~Delegate() =default;

            virtual void replicatorActivityChanged(Replicator*, ActivityLevel) =0;
            virtual void replicatorConnectionClosed(Replicator*, const CloseStatus&) =0;
        };

        ActivityLevel activityLevel() const     {return ReplActor::activityLevel();}

        void stop()                             {enqueue(&Replicator::_stop);}


        // exposed for unit tests:
        websocket::WebSocket* webSocket() const {return connection()->webSocket();}
        alloc_slice checkpointID() const        {return _checkpointDocID;}

        // internal API for Pusher/Puller:
        void updatePushCheckpoint(C4SequenceNumber s)   {_checkpoint.setLocalSeq(s);}
        void updatePullCheckpoint(const alloc_slice &s) {_checkpoint.setRemoteSeq(s);}
        
        /** Called by the Pusher and Puller when they finish their duties. */
        void taskChangedActivityLevel(ReplActor *task, ActivityLevel level) {
            enqueue(&Replicator::_taskChangedActivityLevel, task, level);
        }

    protected:
        // BLIP ConnectionDelegate API:
        virtual void onConnect() override
                                    {enqueue(&Replicator::_onConnect);}
        virtual void onClose(Connection::CloseStatus status) override
                                    {enqueue(&Replicator::_onClose, status);}
        virtual void onRequestReceived(blip::MessageIn *msg) override
                                    {enqueue(&Replicator::_onRequestReceived,
                                             Retained<blip::MessageIn>(msg));}

    private:
        Replicator(C4Database*, const websocket::Address&, Delegate&, Options, Connection*);
        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(Connection::CloseStatus);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void _stop();
        void getCheckpoints();
        void startReplicating();
        void _taskChangedActivityLevel(ReplActor *task, ActivityLevel);
        virtual ActivityLevel computeActivityLevel() const override;
        virtual void activityLevelChanged(ActivityLevel) override;

        void updateCheckpoint();
        void saveCheckpoint(alloc_slice json)       {enqueue(&Replicator::_saveCheckpoint, json);}
        void _saveCheckpoint(alloc_slice json);

        const websocket::Address _remoteAddress;
        CloseStatus _closeStatus;
        Delegate& _delegate;
        ActivityLevel _pushActivity, _pullActivity, _dbActivity;
        Retained<DBActor> _dbActor;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;

        Checkpoint _checkpoint;
        alloc_slice _checkpointDocID;
        alloc_slice _checkpointRevID;
    };

} }
