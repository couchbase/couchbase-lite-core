//
// Checkpoint.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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
#include "Logging.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"

#define SPARSE_CHECKPOINTS  // Under construction -- don't enable this yet

namespace litecore { namespace repl {
    using namespace std;
    using namespace fleece;


    bool Checkpoint::gWriteTimestamps = true;


    void Checkpoint::readJSON(slice json) {
        if (json) {
            Doc root = Doc::fromJSON(json, nullptr);
            _minSequence = (C4SequenceNumber) root["local"_sl].asInt();
            _remote = root["remote"_sl].toJSON();
            _maxSequence = _minSequence;
            _pending.clear();
#ifdef SPARSE_CHECKPOINTS
            // New properties for sparse checkpoint:
            Array pending = root["localPending"].asArray();
            for (Array::iterator i(pending); i; ++i) {
                C4SequenceNumber first = i->asInt();
                C4SequenceNumber last = first + (++i)->asInt() - 1;
                _pending.add(first, last+1);
            }
            _maxSequence = max(_maxSequence, _pending.last());
            C4SequenceNumber localMax = root["localMax"].asInt();
            if (localMax)
                _maxSequence = localMax;
#endif
        } else {
            _minSequence = 0;
            _remote = nullslice;
            _pending.clear();
            _maxSequence = 0;
        }
    }


    alloc_slice Checkpoint::toJSON() const {
        JSONEncoder enc;
        enc.beginDict();
        if (_minSequence) {
            enc.writeKey("local"_sl);
            enc.writeUInt(_minSequence);
        }
        if (_remote) {
            enc.writeKey("remote"_sl);
            enc.writeRaw(_remote);   // remote is already JSON
        }
        if (gWriteTimestamps) {
            enc.writeKey("time"_sl);
            enc.writeInt(c4_now() / 1000);
        }

#ifdef SPARSE_CHECKPOINTS
        if (!_pending.empty()) {
            // New properties for sparse checkpoint:
            enc.writeKey("localPending"_sl);
            enc.beginArray();
            for (auto &range : _pending) {
                enc.writeInt(range.first);
                enc.writeInt(range.second - range.first);
            };
            enc.endArray();
            if (_maxSequence > _pending.last()) {
                enc.writeKey("localMax"_sl);
                enc.writeInt(_maxSequence);
            }
        }
#endif
        enc.endDict();
        return enc.finish();
    }


    bool Checkpoint::validateWith(const Checkpoint &remoteSequences) {
        bool match = true;
        if (_minSequence > 0 && _minSequence != remoteSequences._minSequence) {
            LogTo(SyncLog, "Local sequence mismatch: I had %llu, remote had %llu",
                  (unsigned long long)_minSequence,
                  (unsigned long long)remoteSequences._minSequence);
            _minSequence = 0;
            match = false;
        }
        if (_remote && _remote != remoteSequences._remote) {
            LogTo(SyncLog, "Remote sequence mismatch: I had '%.*s', remote had '%.*s'",
                  SPLAT(_remote), SPLAT(remoteSequences._remote));
            _remote = nullslice;
            match = false;
        }
        return match;
    }


    bool Checkpoint::setRemoteMinSequence(fleece::slice s) {
        if (s == _remote)
            return false;
        _remote = s;
        return true;
    }



    bool Checkpoint::isSequencePending(C4SequenceNumber seq) const {
        return seq > _minSequence && (seq > _maxSequence || _pending.contains(seq));
    }


    void Checkpoint::addPendingSequence(C4SequenceNumber seq) {
        _pending.add(seq);
        // adding a pending seq can't bump checkpoint, so no need to call updateLocalFromPending
    }


    void Checkpoint::completedSequence(C4SequenceNumber seq) {
        _pending.remove(seq);
        updateLocalFromPending();
    }


    void Checkpoint::updateLocalFromPending() {
        auto lastComplete = _pending.empty() ? _maxSequence : _pending.first() - 1;
        _minSequence = max(_minSequence, lastComplete);
    }

} }
