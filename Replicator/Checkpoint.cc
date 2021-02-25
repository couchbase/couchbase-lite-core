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

#define SPARSE_CHECKPOINTS  // If defined, save entire sparse set to JSON

namespace litecore { namespace repl {
    using namespace std;
    using namespace fleece;


    bool Checkpoint::gWriteTimestamps = true;


    void Checkpoint::resetLocal() {
        _completed.clear();
        _completed.add(0, 1);
        _lastChecked = 0;
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
        if (_completed.rangesCount() > 1) {
            // New property for sparse checkpoint. Write the pending sequence ranges as
            // (sequence, length) pairs in an array, omitting the 'infinity' at the end of the last.
            enc.writeKey("localCompleted"_sl);
            enc.beginArray();
            for (auto &range : _completed) {
                enc.writeInt(range.first);
                enc.writeInt(range.second - range.first);
            };
            enc.endArray();
        }
#endif

        if (_remote) {
            enc.writeKey("remote"_sl);
            enc.writeRaw(_remote.toJSON());
        }
        
        enc.endDict();
        return enc.finish();
    }


    void Checkpoint::readJSON(slice json) {
        resetLocal();
        _remote = {};
        if (json) {
            Doc root = Doc::fromJSON(json, nullptr);
            if (!root) {
                LogError(SyncLog, "Unparseable checkpoint: %.*s", SPLAT(json));
                return;
            }
            _remote = RemoteSequence(root["remote"_sl]);

#ifdef SPARSE_CHECKPOINTS
            // New properties for sparse checkpoint:
            Array pending = root["localCompleted"].asArray();
            if (pending) {
                for (Array::iterator i(pending); i; ++i) {
                    C4SequenceNumber first = i->asInt();
                    C4SequenceNumber last = (++i)->asInt();
                    if (last >= first)
                        _completed.add(first, last + 1);
                }
            } else
#endif
            {
                auto minSequence = (C4SequenceNumber) root["local"_sl].asInt();
                _completed.add(0, minSequence + 1);
            }
        }
    }


    bool Checkpoint::validateWith(const Checkpoint &remoteSequences) {
        bool match = true;
        if (_completed != remoteSequences._completed) {
            LogTo(SyncLog, "Local sequence mismatch: I had completed: %s, remote had %s",
                  _completed.to_string().c_str(),
                  remoteSequences._completed.to_string().c_str());
            resetLocal();
            match = false;
        }
        if (_remote && _remote != remoteSequences._remote) {
            LogTo(SyncLog, "Remote sequence mismatch: I had '%s', remote had '%s'",
                  _remote.toJSONString().c_str(), remoteSequences._remote.toJSONString().c_str());
            _remote = {};
            match = false;
        }
        return match;
    }


    C4SequenceNumber Checkpoint::localMinSequence() const {
        assert(!_completed.empty());
        return _completed.begin()->second - 1;
    }


    void Checkpoint::addPendingSequence(C4SequenceNumber s) {
        _lastChecked = max(_lastChecked, s);
        _completed.remove(s);
    }



    size_t Checkpoint::pendingSequenceCount() const {
        // Count the gaps between the ranges:
        size_t count = 0;
        C4SequenceNumber end = 0;
        for (auto &range : _completed) {
            count += range.first - end;
            end = range.second;
        }
        if (_lastChecked > end - 1)
            count += _lastChecked - (end - 1);
        return count;
    }


    bool Checkpoint::setRemoteMinSequence(const RemoteSequence &s) {
        if (s == _remote)
            return false;
        _remote = s;
        return true;
    }

} }



namespace litecore {

    // The one SequenceSet method I didn't want in the header (because it drags in <stringstream>)

    std::string SequenceSet::to_string() const {
        std::stringstream str;
        str << "[";
        int n = 0;
        for (auto &range : _sequences) {
            if (n++ > 0) str << ", ";
            str << range.first;
            if (range.second != range.first + 1)
                str << "-" << (range.second - 1);
        }
        str << "]";
        return str.str();
    }

}
