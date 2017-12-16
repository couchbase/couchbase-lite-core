//
//  Pusher.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Replicator.hh"
#include "DBWorker.hh"
#include "Actor.hh"
#include "SequenceSet.hh"
#include <queue>

namespace litecore { namespace repl {

    class Pusher : public Worker {
    public:
        Pusher(blip::Connection *connection, Replicator *replicator, DBWorker *dbActor, Options options);

        // Starts an active push
        void start(C4SequenceNumber sinceSequence)  {enqueue(&Pusher::_start, sinceSequence);}

        // Sent by Replicator in response to dbGetChanges
        void gotChanges(RevList chgs, C4Error err)  {enqueue(&Pusher::_gotChanges, chgs, err);
        }

    protected:
        virtual std::string loggingClassName() const override {return "Push";}

    private:
        Replicator* replicator() const                  {return (Replicator*)_parent.get();}
        void _start(C4SequenceNumber sinceSequence);
        bool passive() const                         {return _options.push <= kC4Passive;}
        virtual ActivityLevel computeActivityLevel() const override;
        void startSending(C4SequenceNumber sinceSequence);
        void handleSubChanges(Retained<blip::MessageIn> req);
        void _gotChanges(RevList changes, C4Error err);
        void sendChanges(RevList);
        void maybeGetMoreChanges();
        void sendChangeList(RevList);
        void maybeSendMoreRevs();
        void sendRevision(const RevRequest&);
        void doneWithRev(const Rev&, bool successful);
        void handleGetAttachment(Retained<MessageIn>);
        void handleProveAttachment(Retained<MessageIn>);
        void _attachmentSent();
        C4ReadStream* readBlobFromRequest(MessageIn *req, slice &digest, C4Error *outError);
        void filterByDocIDs(fleeceapi::Array docIDs);

        static const unsigned kMaxPossibleAncestorsToSend = 20;
        static const unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kMaxChangeListsInFlight = 4;    // How many changes messages can be active at once
        static const bool kChangeMessagesAreUrgent = true;    // Are change msgs high priority?
        static const unsigned kMaxRevsInFlight = 5;           // max # revs to be transmitting at once
        static const unsigned kMaxRevBytesAwaitingReply = 2*1024*1024;     // max bytes of revs sent but not replied
        static const unsigned kDefaultMaxHistory = 20;      // If "changes" response doesn't have one

        Retained<DBWorker> _dbWorker;
        unsigned _changesBatchSize {kDefaultChangeBatchSize};   // # changes to get from db
        DocIDSet _docIDs;
        bool _continuous;
        bool _skipDeleted;
        bool _proposeChanges {false};
        bool _proposeChangesKnown {false};

        C4SequenceNumber _lastSequence {0};       // Checkpointed last-sequence
        bool _gettingChanges {false};             // Waiting for _gotChanges() call?
        SequenceSet _pendingSequences;            // Sequences rcvd from db but not pushed yet
        C4SequenceNumber _lastSequenceRead {0};   // Last sequence read from db
        bool _started {false};
        bool _caughtUp {false};                   // Received backlog of existing changes?
        unsigned _changeListsInFlight {0};        // # change lists being requested from db or sent to peer
        unsigned _revisionsInFlight {0};          // # 'rev' messages being sent
        MessageSize _revisionBytesAwaitingReply {0}; // # 'rev' message bytes sent but not replied
        unsigned _blobsInFlight {0};              // # of blobs being sent
        std::deque<RevRequest> _revsToSend;       // Revs to send to peer but not sent yet
    };
    
    
} }
