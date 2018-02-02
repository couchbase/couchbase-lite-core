//
// Checkpoint.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "Timer.hh"
#include "c4.h"
#include "slice.hh"
#include <chrono>
#include <mutex>

namespace litecore { namespace repl {

    class Checkpoint {
    public:
        struct Sequences {
            C4SequenceNumber local;
            fleece::alloc_slice remote;
        };

        /** Returns my local and remote sequences. */
        Sequences sequences() const;

        /** Sets my local sequence without affecting the remote one. */
        void setLocalSeq(C4SequenceNumber s)        {set(&s, nullptr);}

        /** Sets my remote sequence without affecting the local one. */
        void setRemoteSeq(fleece::slice s)          {set(nullptr, &s);}

        /** Sets my state from an encoded JSON representation. */
        void decodeFrom(fleece::slice json);

        /** Returns a JSON representation of my current state. */
        fleece::alloc_slice encode() const;

        /** Compares my state with another Checkpoint. If the local sequences differ, mine
            will be reset to 0; if the remote sequences differ, mine will be reset to empty. */
        bool validateWith(const Checkpoint&);

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
        void saved();

        /** Returns true if the checkpoint has changes that haven't been saved yet. */
        bool isUnsaved() const;

    private:
        fleece::alloc_slice _encode() const;
        void set(const C4SequenceNumber *local, const fleece::slice *remote);

        std::mutex _mutex;

        Sequences _seq {};

        bool _changed  {false};
        bool _saving {false};
        bool _overdueForSave {false};
        std::unique_ptr<actor::Timer> _timer;
        SaveCallback _saveCallback;
        duration _saveTime;
    };

} }
