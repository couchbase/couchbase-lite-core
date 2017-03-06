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
        /** Constructor for a client connection; will open the Connection itself. */
        Replicator(C4Database*,
                   websocket::Provider&,
                   const websocket::Address&,
                   Options);

        /** Constructor for an incoming (server) connection. */
        Replicator(C4Database*,
                   websocket::WebSocket*,
                   const websocket::Address&,
                   Options = Options::passive());

#if DEBUG
        websocket::WebSocket* webSocket() const {return connection()->webSocket();}
        alloc_slice checkpointID() const        {return _checkpointDocID;}
#endif

        void updatePushCheckpoint(C4SequenceNumber s)   {_checkpoint.setLocalSeq(s);}
        void updatePullCheckpoint(const alloc_slice &s) {_checkpoint.setRemoteSeq(s);}
        
        /** Called by the Pusher and Puller when they finish their duties. */
        void taskComplete(bool isPush) {
            enqueue(&Replicator::_taskComplete, isPush);
        }

    protected:
        // BLIP ConnectionDelegate API:
        virtual void onConnect() override
                                    {enqueue(&Replicator::_onConnect);}
        virtual void onClose(bool normalClose, int status, fleece::slice reason) override
                                    {enqueue(&Replicator::_onClose,
                                             normalClose, status, alloc_slice(reason));}
        virtual void onRequestReceived(blip::MessageIn *msg) override
                                    {enqueue(&Replicator::_onRequestReceived,
                                             Retained<blip::MessageIn>(msg));}

        virtual void afterEvent() override;
    private:
        Replicator(C4Database*, const websocket::Address&, Options, Connection*);
        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(bool normalClose, int status, fleece::alloc_slice reason);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void getCheckpoints();
        void startReplicating();
        void _taskComplete(bool isPush);

        void updateCheckpoint();
        void saveCheckpoint(alloc_slice json);

        const websocket::Address _remoteAddress;
        bool _pushing, _pulling;
        Retained<DBActor> _dbActor;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;

        Checkpoint _checkpoint;
        alloc_slice _checkpointDocID;
        alloc_slice _checkpointRevID;
    };

} }
