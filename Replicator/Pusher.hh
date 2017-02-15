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
#include "Base.hh"

namespace litecore { namespace repl {


    class Pusher : public Actor {
    public:
        Pusher(Replicator *replicator, bool continuous, sequence_t sinceSequence)
        :_replicator(replicator)
        ,_continuous(continuous)
        ,_lastSequence(sinceSequence)
        { }

        void start();

        void databaseChanged(ChangeList cl)     {enqueue(&Pusher::_databaseChanged, cl);}

    private:
        void _databaseChanged(ChangeList cl);
        void sendChangesSince(sequence_t since);
        void sendChangeList(const ChangeList&);

        static const unsigned kMaxPossibleAncestorsToSend = 20;
        static const unsigned kMinLengthToCompress = 100;     // Min length body worth compressing
        static const unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kMaxChangeMessagesInFlight = 4; // How many changes messages can be active at once
        static const bool kChangeMessagesAreUrgent = true;    // Are change msgs high priority?
        constexpr static const float kProgressUpdateInterval = 0.25;    // How often to update self.progress

        Replicator* const _replicator;
        bool const _continuous;
        sequence_t _lastSequence;
        unsigned _changesBatchSize {kDefaultChangeBatchSize};
    };
    
    
} }
