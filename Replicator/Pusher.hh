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

namespace litecore { namespace repl {


    class Pusher : public ReplActor {
    public:
        Pusher(Replicator *replicator, DBActor *dbActor);

        void start(C4SequenceNumber sinceSequence, bool continuous);

        // Sent by Replicator in response to dbGetChanges
        void gotChanges(RevList changes, C4Error err) {
            enqueue(&Pusher::_gotChanges, changes, err);
        }

    private:
        void _gotChanges(RevList changes, C4Error err);
        void getMoreChanges();
        void sendChangeList(RevList);
        void sendRevision(const Rev&,
                          const std::vector<std::string> &ancestors,
                          unsigned maxHistory);

        static const unsigned kMaxPossibleAncestorsToSend = 20;
        static const unsigned kMinLengthToCompress = 100;     // Min length body worth compressing
        static const unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kMaxChangeListsInFlight = 4;    // How many changes messages can be active at once
        static const bool kChangeMessagesAreUrgent = true;    // Are change msgs high priority?
        constexpr static const float kProgressUpdateInterval = 0.25;    // How often to update self.progress

        Replicator* const _replicator;
        DBActor* const _dbActor;
        bool _continuous;
        unsigned _changesBatchSize {kDefaultChangeBatchSize};   // # changes to get from db

        C4SequenceNumber _lastSequence {0};             // Checkpointed last-sequence
        C4SequenceNumber _lastSequenceSent {0};         // Last sequence sent in 'changes' msg
        C4SequenceNumber _lastSequenceRequested {0};    // Last sequence requested from db
        unsigned _changeListsInFlight {0};              // # 'changes' msgs pending replies
    };
    
    
} }
