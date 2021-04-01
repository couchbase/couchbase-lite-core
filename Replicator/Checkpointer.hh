//
// Checkpointer.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "ReplicatorTypes.hh"
#include "Error.hh"
#include "Timer.hh"
#include "c4Base.hh"
#include "fleece/slice.hh"
#include "URLTransformer.hh"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

struct C4UUID;

namespace litecore {
    class Logging;
}

namespace litecore { namespace repl {
    using namespace fleece;

    class Checkpoint;
    struct Options;
    class RemoteSequence;


    /** Manages a Replicator's checkpoint, including local storage (but not remote).
        \note The checkpoint-access methods are thread-safe since they are called by the
              Replicator, Pusher and Puller. */
    class Checkpointer {
    public:
        Checkpointer(const Options&, fleece::slice remoteURL);

        ~Checkpointer();

        // Checkpoint:

        /** Compares my state with another Checkpoint.
            If the local sequences differ, mine will be reset to 0;
            if the remote sequences differ, mine will be reset to empty. */
        bool validateWith(const Checkpoint&);

        std::string to_string() const;

        /** The checkpoint's local sequence. All sequences up to this are pushed. */
        C4SequenceNumber localMinSequence() const;

        void addPendingSequence(C4SequenceNumber);
        void addPendingSequences(const std::vector<C4SequenceNumber> &sequences,
                                 C4SequenceNumber firstInRange,
                                 C4SequenceNumber lastInRange);
        void addPendingSequences(RevToSendList &sequences,
                                 C4SequenceNumber firstInRange,
                                 C4SequenceNumber lastInRange);
        size_t pendingSequenceCount() const;

        void completedSequence(C4SequenceNumber);
        bool isSequenceCompleted(C4SequenceNumber) const;

        /** The checkpoint's remote sequence, the last one up to which all is pulled. */
        RemoteSequence remoteMinSequence() const;

        /** Updates the checkpoint's remote sequence. */
        void setRemoteMinSequence(const RemoteSequence&);

        // Checkpoint IDs:

        /** Returns the doc ID where the checkpoint should initially be read from.
            This is usually the same as \ref checkpointID, but not in the case of a copied
            database that's replicating for the first time. */
        slice initialCheckpointID() const       {Assert(_initialDocID); return _initialDocID;}

        /** Returns the doc ID where the checkpoint is to be stored. */
        alloc_slice checkpointID() const        {Assert(_docID); return _docID;}

        /** The actual JSON read from the local checkpoint.
            (Kept around for logging. Only available until the checkpoint changes.) */
        slice checkpointJSON() const            {return _checkpointJSON;}

        /** The identifier to use for the remote database; either its URL or a client-provided UID. */
        slice remoteDBIDString() const;

        // Database I/O:

        /** Reads the checkpoint state from the local database. This needs to happen first.
            If the checkpoint has already been read, this is a no-op.
            Returns false if the checkpoint wasn't read. Other errors are thrown as exceptions. */
        bool read(C4Database *db NONNULL, bool reset);

        /** Writes serialized checkpoint state to the local database.
            Does not write the current checkpoint state, because it may have changed since the
            remote save. It's important that the saved data be the same as what was saved on
            the remote peer. */
        void write(C4Database *db NONNULL, slice checkpointData);

        // Autosave:

        using duration = std::chrono::nanoseconds;
        using SaveCallback = std::function<void(fleece::alloc_slice jsonToSave)>;

        /** Enables autosave: at about the given duration after the first change is made,
            the callback will be invoked, and passed a JSON representation of my state. */
        void enableAutosave(duration saveTime, SaveCallback cb);

        /** Disables autosave. Returns true if no more calls to save() will be made. The only
            case where another call to save() might be made is if a save is currently in
            progress, and the checkpoint has been changed since the save began. In that case,
            another save will have to be triggered immediately when the current one finishes. */
        void stopAutosave();

        /** Triggers an immediate save, if the checkpoint has changed. */
        bool save();

        /** The client should call this as soon as its save completes, which can be after the
            SaveCallback returns. */
        void saveCompleted();

        /** Returns true if the checkpoint has changes that haven't been saved yet. */
        bool isUnsaved() const;

        // Pending documents:

        using PendingDocCallback = function_ref<void(const C4DocumentInfo&)>;

        /** Calls the callback for each document that has revisions pending push */
        void pendingDocumentIDs(C4Database* NONNULL, PendingDocCallback);

        /** Checks if the document with the given ID has any pending revisions to push*/
        bool isDocumentPending(C4Database* NONNULL, slice docId);

        bool isDocumentAllowed(C4Document* doc NONNULL);
        bool isDocumentIDAllowed(slice docID);

        // Peer checkpoint access (for passive replicator):

        /// Returns false if no checkpoint exists.
        static bool getPeerCheckpoint(C4Database* NONNULL,
                                      slice checkpointID,
                                      alloc_slice &outBody,
                                      alloc_slice &outRevID);

        /// Returns false if revID conflicts.
        static bool savePeerCheckpoint(C4Database* NONNULL,
                                       slice checkpointID,
                                       slice body,
                                       slice revID,
                                       alloc_slice &newRevID);

    private:
        void checkpointIsInvalid();
        std::string docIDForUUID(const C4UUID&, URLTransformStrategy strategy);
        slice remoteDocID(C4Database *db NONNULL);
        alloc_slice _read(C4Database *db NONNULL, slice);
        void initializeDocIDs();
        void saveSoon();

        Logging*                        _logger;
        const Options&                  _options;
        alloc_slice const               _remoteURL;
        std::unordered_set<std::string> _docIDs;

        // Checkpoint state:
        mutable std::mutex              _mutex;
        std::unique_ptr<Checkpoint>     _checkpoint;
        alloc_slice                     _checkpointJSON;

        // Document IDs:
        alloc_slice                     _initialDocID;      // DocID checkpoints are read from
        alloc_slice                     _docID;             // Actual checkpoint docID

        // Autosave:
        bool                            _changed  {false};
        bool                            _saving {false};
        bool                            _overdueForSave {false};
        std::unique_ptr<actor::Timer>   _timer;
        SaveCallback                    _saveCallback;
        duration                        _saveTime;
    };

} }
