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
#include <limits>
#include <sstream>

#define SPARSE_CHECKPOINTS  // Under construction -- don't enable this yet

namespace litecore { namespace repl {
    using namespace std;
    using namespace fleece;


    bool Checkpoint::gWriteTimestamps = true;


    static constexpr auto InfinitySequence = numeric_limits<SequenceSet::sequence>::max() - 1;


    Checkpoint::Checkpoint() {
        resetLocal();
    }


    void Checkpoint::resetLocal() {
        _pending.clear();
        _pending.add(1, InfinitySequence);
    }


    alloc_slice Checkpoint::toJSON() const {
        JSONEncoder enc;
        enc.beginDict();
        if (gWriteTimestamps) {
            enc.writeKey("time"_sl);
            enc.writeInt(c4_now() / 1000);
        }

        auto minSeq = localMinSequence();
        if (minSeq > 0) {
            enc.writeKey("local"_sl);
            enc.writeUInt(minSeq);
        }

#ifdef SPARSE_CHECKPOINTS
        if (!_pending.empty() && _pending.begin()->second < InfinitySequence) {
            // New property for sparse checkpoint. Write the pending sequence ranges as
            // (sequence, length) pairs in an array, omitting the 'infinity' at the end of the last.
            enc.writeKey("localPending"_sl);
            enc.beginArray();
            for (auto &range : _pending) {
                enc.writeInt(range.first);
                if (range.second < InfinitySequence)
                    enc.writeInt(range.second - range.first);
            };
            enc.endArray();
        }
#endif

        if (_remote) {
            enc.writeKey("remote"_sl);
            enc.writeRaw(_remote);   // remote is already JSON
        }
        
        enc.endDict();
        return enc.finish();
    }


    void Checkpoint::readJSON(slice json) {
        _pending.clear();
        if (json) {
            Doc root = Doc::fromJSON(json, nullptr);
            _remote = root["remote"_sl].toJSON();

#ifdef SPARSE_CHECKPOINTS
            // New properties for sparse checkpoint:
            Array pending = root["localPending"].asArray();
            if (pending) {
                for (Array::iterator i(pending); i; ++i) {
                    C4SequenceNumber first = i->asInt();
                    ++i;
                    C4SequenceNumber last = i ? (first + i->asInt()) : InfinitySequence;
                    _pending.add(first, last);
                }
            } else
#endif
            {
                auto minSequence = (C4SequenceNumber) root["local"_sl].asInt();
                _pending.add(minSequence + 1, InfinitySequence);
            }
        } else {
            _remote = nullslice;
            resetLocal();
        }

    }


    bool Checkpoint::validateWith(const Checkpoint &remoteSequences) {
        bool match = true;
        if (_pending != remoteSequences._pending) {
            LogTo(SyncLog, "Local sequence mismatch: I had %s, remote had %s",
                  _pending.to_string().c_str(),
                  remoteSequences._pending.to_string().c_str());
            resetLocal();
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


    C4SequenceNumber Checkpoint::localMinSequence() const {
        assert(!_pending.empty());
        return _pending.first() - 1;
    }


    bool Checkpoint::isSequencePending(C4SequenceNumber seq) const {
        return _pending.contains(seq);
    }


    void Checkpoint::addPendingSequence(C4SequenceNumber seq) {
        _pending.add(seq);
        LogTo(SyncLog, "$$$ ADDED %llu, PENDING: %s", seq, _pending.to_string().c_str());//TEMP
    }


    void Checkpoint::completedSequence(C4SequenceNumber seq) {
        _pending.remove(seq);
        LogTo(SyncLog, "$$$ COMPLETED %llu, PENDING: %s", seq, _pending.to_string().c_str());//TEMP
    }


    bool Checkpoint::setRemoteMinSequence(fleece::slice s) {
        if (s == _remote)
            return false;
        _remote = s;
        return true;
    }

} }


namespace litecore {

    std::string SequenceSet::to_string() const {
        std::stringstream str;
        str << "{";
        int n = 0;
        for (auto &range : _sequences) {
            if (n++ > 0) str << ", ";
            str << range.first;
            if (range.second != range.first + 1) {
                str << "-";
                if (range.second < repl::InfinitySequence)
                    str << (range.second - 1);
            }
        }
        str << "}";
        return str.str();
    }

}
