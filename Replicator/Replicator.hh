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
#include "c4.h"

using namespace litecore::blip;

namespace litecore { namespace repl {

    class Pusher;
    class Puller;


    extern LogDomain SyncLog;

    static inline slice asSlice(C4Slice s) {return {s.buf, s.size};}
    static inline C4Slice asSlice(slice s) {return {s.buf, s.size};}

    
    struct ChangeEntry {
        sequence_t sequence;
        alloc_slice docID;
        alloc_slice revID;
        C4DocumentFlags flags;
        ChangeEntry(const C4DocumentInfo &info)
        :sequence(info.sequence), docID(asSlice(info.docID)), revID(asSlice(info.revID)),
         flags(info.flags) { }
    };

    struct ChangeList {
        std::vector<ChangeEntry> changes;
        C4Error error = {};
    };


    /** The top-level replicator object, which runs the BLIP connection.
        It also has direct access to the C4Database.
        Pull and push operations are run by subidiary Puller and Pusher objects. */
    class Replicator : public Actor, ConnectionDelegate {
    public:
        struct Options {
            bool push {false};
            bool pull {false};
            bool continuous {false};
        };

        Replicator(C4Database*, websocket::Provider&, const websocket::Address&, Options);

        FutureResponse sendRequest(MessageBuilder& builder) {
            return connection()->sendRequest(builder);
        }

        // Database access:
        
        Retained<Future<ChangeList>> dbGetChanges(sequence_t since, unsigned limit);


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
            sequence_t localSeq {0};
            std::string remoteSeq;
        };

        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(int status, fleece::alloc_slice reason);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void gotError(const MessageIn* msg);
        void getCheckpoint();
        void handleGetCheckpoint(Retained<MessageIn>);
        std::string effectiveRemoteCheckpointDocID();
        Checkpoint decodeCheckpoint(slice json);
        void startReplicating();

        void _dbGetChanges(sequence_t since, unsigned limit, Retained<Future<ChangeList>> promise);

        C4Database *_db;
        const websocket::Address _remoteAddress;
        const Options _options;
        Retained<Connection> _connection;
        std::string _remoteCheckpointDocID;
        Checkpoint _checkpoint;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;

        typedef void (Replicator::*Handler)(Retained<MessageIn>);
        struct HandlerEntry {const char *profile; Replicator::Handler handler;};
        static const HandlerEntry kHandlers[];
    };

} }
