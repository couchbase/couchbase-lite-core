//
//  Replicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "ReplActor.hh"
#include "BLIPConnection.hh"
#include "FleeceCpp.hh"

using namespace litecore::blip;

namespace litecore { namespace repl {

    class Pusher;
    class Puller;
    class DBActor;


    /** The top-level replicator object, which runs the BLIP connection.
        It also has direct access to the C4Database.
        Pull and push operations are run by subidiary Puller and Pusher objects. */
    class Replicator : public ReplActor, ConnectionDelegate {
    public:
        struct Options {
            bool push;
            bool pull;
            bool continuous;
        };

        /** Constructor for a client connection; will open the Connection itself. */
        Replicator(C4Database*, websocket::Provider&, const websocket::Address&, Options);

        /** Constructor for an incoming connection. */
        Replicator(C4Database*, blip::Connection*, const websocket::Address&);

        Connection* connection() const {return ConnectionDelegate::connection();} // disambiguation

#if DEBUG
        websocket::WebSocket* webSocket() const {return connection()->webSocket();}
#endif

    protected:
        // BLIP ConnectionDelegate API:
        virtual void onConnect() override
                                    {enqueue(&Replicator::_onConnect);}
        virtual void onError(int errcode, fleece::slice reason) override
                                    {enqueue(&Replicator::_onError, errcode, alloc_slice(reason));}
        virtual void onClose(int status, fleece::slice reason) override
                                    {enqueue(&Replicator::_onClose, status, alloc_slice(reason));}
        virtual void onRequestReceived(blip::MessageIn *msg) override
                                    {enqueue(&Replicator::_onRequestReceived,
                                             Retained<blip::MessageIn>(msg));}

    private:
        struct Checkpoint {
            C4SequenceNumber localSeq {0};
            std::string remoteSeq;
        };

        Replicator(C4Database*, const websocket::Address&, Options, Connection*);
        void setConnection(Connection*) override;
        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(int status, fleece::alloc_slice reason);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void getCheckpoints();
        Checkpoint decodeCheckpoint(slice json);
        void startReplicating();

        const websocket::Address _remoteAddress;
        const Options _options;
        Retained<DBActor> _dbActor;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;
        Checkpoint _checkpoint;
    };

} }
