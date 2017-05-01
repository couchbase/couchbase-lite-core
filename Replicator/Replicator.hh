//
//  Replicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include "Checkpoint.hh"
#include "BLIPConnection.hh"
#include "FleeceCpp.hh"
#include "Benchmark.hh"

using namespace litecore::blip;

namespace litecore { namespace repl {

    class Pusher;
    class Puller;
    class DBWorker;


    /** The top-level replicator object, which runs the BLIP connection.
        Pull and push operations are run by subidiary Puller and Pusher objects.
        The database will only be accessed by the DBAgent object. */
    class Replicator : public Worker, ConnectionDelegate {
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
                   Delegate&,
                   Options = Options::passive());

        /** Replicator delegate; receives progress & error notifications. */
        class Delegate {
        public:
            virtual ~Delegate() =default;

            virtual void replicatorStatusChanged(Replicator*, const Status&) =0;
            virtual void replicatorConnectionClosed(Replicator*, const CloseStatus&)  { }
        };

        Status status() const                   {return Worker::status();}   //FIX: Needs to be thread-safe

        void stop()                             {enqueue(&Replicator::_stop);}


        // exposed for unit tests:
        websocket::WebSocket* webSocket() const {return connection()->webSocket();}
        alloc_slice checkpointID() const        {return _checkpointDocID;}

        // internal API for Pusher/Puller:
        void updatePushCheckpoint(C4SequenceNumber s)   {_checkpoint.setLocalSeq(s);}
        void updatePullCheckpoint(const alloc_slice &s) {_checkpoint.setRemoteSeq(s);}
        
    protected:
        // BLIP ConnectionDelegate API:
        virtual void onConnect() override
                                                {enqueue(&Replicator::_onConnect);}
        virtual void onClose(Connection::CloseStatus status, Connection::State state) override
                                                {enqueue(&Replicator::_onClose, status, state);}
        virtual void onRequestReceived(blip::MessageIn *msg) override
                                        {enqueue(&Replicator::_onRequestReceived, retained(msg));}
        virtual void changedStatus() override;

    private:
        // How long to wait between delegate calls when only the progress % has changed
        static constexpr double kMinDelegateCallInterval = 0.2;

        Replicator(C4Database*, const websocket::Address&, Delegate&, Options, Connection*);
        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(Connection::CloseStatus, Connection::State);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void _stop();
        void getCheckpoints();
        void startReplicating();
        virtual ActivityLevel computeActivityLevel() const override;
        void reportStatus();

        void updateCheckpoint();
        void saveCheckpoint(alloc_slice json)       {enqueue(&Replicator::_saveCheckpoint, json);}
        void _saveCheckpoint(alloc_slice json);

        virtual void _childChangedStatus(Worker *task, Status taskStatus) override;

        const websocket::Address _remoteAddress;
        CloseStatus _closeStatus;
        Delegate* _delegate;
        Retained<DBWorker> _dbActor;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;
        Connection::State _connectionState;
        Status _pushStatus {}, _pullStatus {}, _dbStatus {};
        Stopwatch _sinceDelegateCall;
        ActivityLevel _lastDelegateCallLevel {};
        bool _waitingToCallDelegate {false};

        Checkpoint _checkpoint;
        alloc_slice _checkpointDocID;
        alloc_slice _checkpointRevID;
    };

} }
