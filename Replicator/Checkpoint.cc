//
//  Checkpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Checkpoint.hh"
#include "ReplActor.hh"
#include "Logging.hh"
#include "FleeceCpp.hh"

using namespace fleece;
using namespace fleeceapi;
using namespace std;

namespace litecore { namespace repl {

    alloc_slice Checkpoint::remoteSeq() const {
        lock_guard<mutex> lock(const_cast<Checkpoint*>(this)->_mutex);
        return _remoteSeq;
    }

    void Checkpoint::set(const C4SequenceNumber *local, const slice *remote) {
        lock_guard<mutex> lock(_mutex);
        if (local)
            _localSeq = *local;
        if (remote)
            _remoteSeq = *remote;
        if (_timer) {
            _changed = true;
            _timer->fireAfter(_saveTime);
        }
    }


    bool Checkpoint::validateWith(const Checkpoint &checkpoint) {
        bool match = true;
        if (_localSeq > 0 && _localSeq != checkpoint._localSeq) {
            Log("Local sequence mismatch: I had %llu, remote had %llu",
                _localSeq, checkpoint._localSeq);
            _localSeq = 0;
            match = false;
        }
        if (_remoteSeq && _remoteSeq != checkpoint._remoteSeq) {
            Log("Remote sequence mismatch: I had '%.*s', remote had '%.*s'",
                SPLAT(_remoteSeq), SPLAT(checkpoint._remoteSeq));
            _remoteSeq = nullslice;
            match = false;
        }
        return match;
    }


    // Decodes the JSON body of a checkpoint doc into a Checkpoint struct
    void Checkpoint::decodeFrom(slice json) {
        if (json) {
            alloc_slice f = Encoder::convertJSON(json, nullptr);
            Dict root = Value::fromData(f).asDict();
            _localSeq = (C4SequenceNumber) root["local"_sl].asInt();
            _remoteSeq = root["remote"_sl].toString();
        } else {
            _localSeq = 0;
            _remoteSeq = nullslice;
        }
    }

    
    // Encodes a Checkpoint to JSON
    alloc_slice Checkpoint::encode() const {
        JSONEncoder enc;
        enc.beginDict();
        if (_localSeq) {
            enc.writeKey("local"_sl);
            enc.writeUInt(_localSeq);
        }
        if (_remoteSeq) {
            enc.writeKey("remote"_sl);
            enc.writeString(_remoteSeq);
        }
        enc.endDict();
        return enc.finish();
    }


    void Checkpoint::autosave(std::chrono::milliseconds saveTime, SaveCallback cb) {
        lock_guard<mutex> lock(_mutex);
        _saveCallback = cb;
        _saveTime = saveTime;
        _timer.reset( new Timer( bind(&Checkpoint::save, this) ) );
    }


    void Checkpoint::stopAutosave() {
        lock_guard<mutex> lock(_mutex);
        _timer.reset();
        _changed = false;
    }



    void Checkpoint::save() {
        alloc_slice json;
        {
            lock_guard<mutex> lock(_mutex);
            if (!_changed || !_timer)
                return;
            _changed = false;
            json = encode();
        }
        _saveCallback(json);
    }

} }
