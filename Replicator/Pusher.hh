//
//  Pusher.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Replicator.hh"
#include "Actor.hh"

namespace litecore { namespace repl {


    class Pusher : public Actor {
    public:
        Pusher(Replicator *replicator, bool continuous, C4SequenceNumber sinceSequence);

        void start();

        // Sent by Replicator in response to dbGetChanges
        void gotChanges(RevList changes, C4Error err) {
            enqueue(&Pusher::_gotChanges, changes, err);
        }

    private:
        void _gotChanges(RevList changes, C4Error err);
        void sendMoreChanges();
        void sendChangeList(RevList);
        void gotError(const MessageIn*);
        void gotError(C4Error);
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
        bool const _continuous;
        C4SequenceNumber _lastSequence, _lastSequenceSent, _lastSequenceRequested {0};
        unsigned _changesBatchSize {kDefaultChangeBatchSize};
        unsigned _changeListsInFlight {0};
    };
    
    
} }
