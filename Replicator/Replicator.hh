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
#include "c4.h"

using namespace litecore::blip;

namespace litecore { namespace repl {

    class Pusher;
    class Puller;


    struct Rev {
        alloc_slice docID;
        alloc_slice revID;
        C4SequenceNumber sequence {0};
        C4DocumentFlags flags {0};

        Rev();

        Rev(const C4DocumentInfo &info)
        :sequence(info.sequence), docID(info.docID), revID(info.revID),
         flags(info.flags) { }
    };
    typedef std::vector<Rev> RevList;


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

        FutureResponse sendRequest(MessageBuilder& builder) {
            return connection()->sendRequest(builder);
        }

        // Database access:
        
        void dbGetChanges(C4SequenceNumber since, unsigned limit, bool continuous) {
            enqueue(&Replicator::_dbGetChanges, since, limit, continuous);
        }

        void dbSendRevision(Rev rev, std::vector<std::string> ancestors, int maxHistory) {
            enqueue(&Replicator::_dbSendRevision, rev, ancestors, maxHistory);
        }

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
        static const size_t kMaxPossibleAncestors = 10;
        
        struct Checkpoint {
            C4SequenceNumber localSeq {0};
            std::string remoteSeq;
        };

        void setConnection(Connection*);
        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(int status, fleece::alloc_slice reason);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void handleChanges(Retained<MessageIn>);
        void handleGetCheckpoint(Retained<MessageIn>);

        void getCheckpoint();
        std::string effectiveRemoteCheckpointDocID();
        Checkpoint decodeCheckpoint(slice json);
        void startReplicating();

        void _dbGetChanges(C4SequenceNumber since, unsigned limit, bool continuous);
        void _dbSendRevision(Rev rev, std::vector<std::string> ancestors, int maxHistory);
        void dbChanged();
        static void changeCallback(C4DatabaseObserver* observer, void *context);

        bool _dbFindAncestors(slice docID, slice revID,
                              std::vector<fleece::alloc_slice> &ancestors);

        C4Database *_db;
        const websocket::Address _remoteAddress;
        const Options _options;
        std::string _remoteCheckpointDocID;
        Checkpoint _checkpoint;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;
        C4DatabaseObserver *_changeObserver {nullptr};
    };

} }
