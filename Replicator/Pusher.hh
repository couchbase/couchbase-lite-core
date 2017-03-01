//
//  Pusher.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Replicator.hh"
#include "DBActor.hh"
#include "Actor.hh"
#include "SequenceSet.hh"

namespace litecore { namespace repl {


    class Pusher : public ReplActor {
    public:
        Pusher(blip::Connection *connection, Replicator *replicator, DBActor *dbActor, Options options);

        void start(C4SequenceNumber sinceSequence);

        // Sent by Replicator in response to dbGetChanges
        void gotChanges(RevList changes, C4Error err) {
            enqueue(&Pusher::_gotChanges, changes, err);
        }

    private:
        virtual bool isBusy() const override;
        virtual void afterEvent() override;
        void startSending(C4SequenceNumber sinceSequence);
        void handleSubChanges(Retained<blip::MessageIn> req);
        void _gotChanges(RevList changes, C4Error err);
        blip::FutureResponse sendChanges(const RevList&);
        void maybeGetMoreChanges();
        void sendChangeList(RevList);
        void sendRevision(const Rev&,
                          const std::vector<std::string> &ancestors,
                          unsigned maxHistory);
        void markComplete(C4SequenceNumber sequence);

        static const unsigned kMaxPossibleAncestorsToSend = 20;
        static const unsigned kMinLengthToCompress = 100;     // Min length body worth compressing
        static const unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kMaxChangeListsInFlight = 4;    // How many changes messages can be active at once
        static const bool kChangeMessagesAreUrgent = true;    // Are change msgs high priority?
        constexpr static const float kProgressUpdateInterval = 0.25;    // How often to update self.progress

        Replicator* const _replicator;
        DBActor* const _dbActor;
        unsigned _changesBatchSize {kDefaultChangeBatchSize};   // # changes to get from db

        C4SequenceNumber _lastSequence {0};             // Checkpointed last-sequence
        C4SequenceNumber _lastSequenceSent {0};         // Last sequence sent in 'changes' msg
        bool _gettingChanges {false};                   // Waiting for _gotChanges() call?
        SequenceSet _pendingSequences;
        C4SequenceNumber _lastSequenceRead {0};         // Last sequence read from db
        bool _caughtUp {false};
        unsigned _changeListsInFlight {0};              // # 'changes' msgs pending replies
        unsigned _revisionsInFlight {0};                // # 'rev' msgs pending replies
    };
    
    
} }
