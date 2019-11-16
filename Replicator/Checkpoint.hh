//
// Checkpoint.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "SequenceSet.hh"
#include "Logging.hh"
#include "c4Base.h"
#include "fleece/slice.hh"
#include <algorithm>

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
        Checkpoint();
        Checkpoint(fleece::slice json)                  {readJSON(json);}

        void readJSON(fleece::slice json);
        fleece::alloc_slice toJSON() const;

        void resetLocal();

        bool validateWith(const Checkpoint &remoteSequences);

        C4SequenceNumber    localMinSequence() const;

        const SequenceSet&  pendingSequences() const    {return _pending;}

        bool isSequencePending(C4SequenceNumber) const;

        void addPendingSequence(C4SequenceNumber);

        void completedSequence(C4SequenceNumber seq);

        template <class REV_LIST>
        void addPendingSequences(REV_LIST& revs,
                                 C4SequenceNumber firstSequenceChecked,
                                 C4SequenceNumber lastSequenceChecked)
        {
            _pending.remove(firstSequenceChecked, lastSequenceChecked + 1);
            for (auto rev : revs)
                _pending.add(rev->sequence);
            LogTo(SyncLog, "$$$ AFTER [%llu-%llu], PENDING: %s",
                  firstSequenceChecked, lastSequenceChecked, _pending.to_string().c_str());//TEMP
        }

        fleece::alloc_slice remoteMinSequence() const   {return _remote;}

        bool setRemoteMinSequence(fleece::slice s);

        static bool gWriteTimestamps;   // for testing; set to false to disable timestamps in JSON

    private:
        void updateLocalFromPending();

        SequenceSet         _pending;           // Set of pending sequences between min/max
        fleece::alloc_slice _remote;            // Remote checkpoint (last completed sequence)
    };

} }
