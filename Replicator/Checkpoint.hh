//
// Checkpoint.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "SequenceSet.hh"
#include "RemoteSequence.hh"
#include "c4Base.h"
#include "fleece/slice.hh"
#include <algorithm>
#include <vector>

namespace litecore { namespace repl {

    /**
     * Tracks the state of replication, i.e. which sequences have been sent/received and which
     * haven't. This state is persisted by storing a JSON serialization of the Checkpoint into
     * a pair of documents, one local and one on the server. At the start of replication both
     * documents are read, and if they agree, the replication continues from that state, otherwise
     * it starts over from the beginning.
     *
     * The local (push) state is essentially a set of sequences, represented as three values:
     * - `minSequence`, also just called the "checkpoint". All sequences less than or equal to
     *   this are known to have been pushed.
     * - `maxSequence`, the maximum sequence seen by the pusher. All sequences greater than
     *   this have, obviously, not been pushed.
     * - `pending`, a set of sequences in the range [minSequence, maxSequence) that are known but
     *   have not yet been pushed.
     *
     * The remote (pull) state is simpler, just one sequence. This is a _server-side_ sequence,
     * which is not an integer but a string, known to be JSON-encoded but otherwise opaque.
     * Since these sequences cannot be ordered and may occupy much more space, we don't
     * attempt to keep track of the exact set of pulled sequences. Instead, we just remember a
     * single sequence which has the same interpretation as `minSequence` does: this sequence
     * and all earlier ones are known to have been pulled. That means the replicator can start
     * by asking the server to send only sequences newer than it.
     */
    class Checkpoint {
    public:
        Checkpoint()                                        {resetLocal();}
        Checkpoint(fleece::slice json)                      {readJSON(json);}

        void readJSON(fleece::slice json);
        
        void readDict(fleece::Dict dict);

        fleece::alloc_slice toJSON() const;

        bool validateWith(const Checkpoint &remoteSequences);

        //---- Local sequences:

        /** The last fully-complete local sequence, such that it and all lesser sequences are
            complete. In other words, the sequence before the first pending sequence. */
        C4SequenceNumber    localMinSequence() const;

        /** The set of sequences that have been "completed": either pushed, or skipped, or else
            don't exist. */
        const SequenceSet&  completedSequences() const      {return _completed;}

        /** Has this sequence been completed? */
        bool isSequenceCompleted(C4SequenceNumber s) const  {return _completed.contains(s);}

        /** Removes a sequence from the set of completed sequences. */
        void addPendingSequence(C4SequenceNumber s);

        /** Adds a sequence to the set of completed sequences. */
        void completedSequence(C4SequenceNumber s)          {_completed.add(s);}

        /** Updates the state of a range of sequences:
            All sequences in the range [first...last] are marked completed,
            then the sequences in the collection `revs` are marked uncompleted/pending.*/
        template <class REV_LIST>
        void addPendingSequences(const REV_LIST& revs,
                                 C4SequenceNumber firstSequenceChecked,
                                 C4SequenceNumber lastSequenceChecked)
        {
            assert(lastSequenceChecked >= _lastChecked);
            _lastChecked = lastSequenceChecked;
            _completed.add(firstSequenceChecked, lastSequenceChecked + 1);
            for (auto rev : revs)
                _completed.remove(rev->sequence);
        }

        /** The number of uncompleted sequences up through the last sequence checked. */
        size_t pendingSequenceCount() const;

        //---- Remote sequences:

        /** The last fully-complete _remote_ sequence, such that it and all earlier sequences are
            complete. */
        RemoteSequence remoteMinSequence() const       {return _remote;}

        bool setRemoteMinSequence(const RemoteSequence&);

        static bool gWriteTimestamps;   // for testing; set to false to disable timestamps in JSON

    private:
        void resetLocal();
        void updateLocalFromPending();

        SequenceSet         _completed;         // Set of completed local sequences
        C4SequenceNumber    _lastChecked;       // Last local sequence checked in the db
        RemoteSequence      _remote;            // Last completed remote sequence
    };


    // specialization where REV_LIST is std::vector<C4SequenceNumber>
    template <>
    inline void Checkpoint::addPendingSequences(const std::vector<C4SequenceNumber>& revs,
                                                C4SequenceNumber firstSequenceChecked,
                                                C4SequenceNumber lastSequenceChecked)
    {
        _lastChecked = lastSequenceChecked;
        _completed.add(firstSequenceChecked, lastSequenceChecked + 1);
        for (auto rev : revs)
            _completed.remove(rev);
    }

} }
