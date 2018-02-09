//
// Checkpoint.cc
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

#include "Checkpoint.hh"
#include "StringUtil.hh"
#include "Logging.hh"
#include "FleeceCpp.hh"
#include <assert.h>

using namespace fleece;
using namespace fleeceapi;
using namespace std;

namespace litecore { namespace repl {

#define LOCK()  lock_guard<mutex> lock(const_cast<Checkpoint*>(this)->_mutex)

    Checkpoint::Sequences Checkpoint::sequences() const {
        LOCK();
        return _seq;
    }

    void Checkpoint::set(const C4SequenceNumber *local, const slice *remote) {
        LOCK();
        if (local)
            _seq.local = *local;
        if (remote)
            _seq.remote = *remote;

        if (_timer) {
            _changed = true;
            if (!_saving && !_timer->scheduled())
                _timer->fireAfter(_saveTime);
        }
    }


    bool Checkpoint::validateWith(const Checkpoint &checkpoint) {
        LOCK();
        bool match = true;
        auto itsState = checkpoint.sequences();
        if (_seq.local > 0 && _seq.local != itsState.local) {
            LogTo(SyncLog, "Local sequence mismatch: I had %llu, remote had %llu",
                  (unsigned long long)_seq.local,
                  (unsigned long long)itsState.local);
            _seq.local = 0;
            match = false;
        }
        if (_seq.remote && _seq.remote != itsState.remote) {
            LogTo(SyncLog, "Remote sequence mismatch: I had '%.*s', remote had '%.*s'",
                  SPLAT(_seq.remote), SPLAT(itsState.remote));
            _seq.remote = nullslice;
            match = false;
        }
        return match;
    }


    // Decodes the JSON body of a checkpoint doc into a Checkpoint struct
    void Checkpoint::decodeFrom(slice json) {
        LOCK();
        _seq.local = 0;
        _seq.remote = nullslice;
        if (json) {
            alloc_slice f = Encoder::convertJSON(json, nullptr);
            Dict root = Value::fromData(f).asDict();
            _seq.local = (C4SequenceNumber) root["local"_sl].asInt();
            _seq.remote = root["remote"_sl].toJSON();
        }
    }

    
    // Encodes a Checkpoint to JSON
    alloc_slice Checkpoint::_encode() const {
        JSONEncoder enc;
        enc.beginDict();
        if (_seq.local) {
            enc.writeKey("local"_sl);
            enc.writeUInt(_seq.local);
        }
        if (_seq.remote) {
            enc.writeKey("remote"_sl);
            enc.writeRaw(_seq.remote);   // _seq.remote is already JSON
        }
        enc.endDict();
        return enc.finish();
    }

    alloc_slice Checkpoint::encode() const {
        LOCK();
        return _encode();
    }


    void Checkpoint::enableAutosave(duration saveTime, SaveCallback cb) {
        assert(saveTime > duration(0));
        LOCK();
        _saveCallback = cb;
        _saveTime = saveTime;
        _timer.reset( new actor::Timer( bind(&Checkpoint::save, this) ) );
    }


    void Checkpoint::stopAutosave() {
        LOCK();
        _timer.reset();
        _changed = false;
    }


    bool Checkpoint::save() {
        alloc_slice json;
        {
            LOCK();
            if (!_changed || !_timer)
                return true;
            if (_saving) {
                // Can't save immediately because a save is still in progress.
                // Remember that I'm in this state, so when save finishes I can re-save.
                _overdueForSave = true;
                return false;
            }
            _changed = false;
            _saving = true;
            json = _encode();
        }
        _saveCallback(json);
        return true;
    }


    void Checkpoint::saved() {
        bool saveNow = false;
        {
            LOCK();
            if (_saving) {
                _saving = false;
                if (_overdueForSave)
                    saveNow = true;
                else if (_changed)
                    _timer->fireAfter(_saveTime);
            }
        }
        if (saveNow)
            save();
    }

    bool Checkpoint::isUnsaved() const        {LOCK(); return _changed  || _saving;}


} }
