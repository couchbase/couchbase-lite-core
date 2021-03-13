//
// KeyStore.cc
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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

#include "KeyStore.hh"
#include "Record.hh"
#include "DataFile.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Logging.hh"
#include <chrono>

using namespace std;


namespace litecore {

    Record KeyStore::get(slice key, ContentOption option) const {
        Record rec(key);
        read(rec, ReadBy::Key, option);
        return rec;
    }

    Record KeyStore::get(sequence_t seq, ContentOption option) const {
        Record rec;
        rec.updateSequence(seq);
        read(rec, ReadBy::Sequence, option);
        return rec;
    }

#if ENABLE_DELETE_KEY_STORES
    void KeyStore::deleteKeyStore(Transaction& trans) {
        trans.dataFile().deleteKeyStore(name());
    }
#endif

    void KeyStore::set(Record &rec, bool updateSequence, ExclusiveTransaction &t) {
        if (auto seq = set(RecordUpdate(rec), updateSequence, t); seq > 0) {
            rec.setExists();
            if (updateSequence)
                rec.updateSequence(seq);
            else
                rec.updateSubsequence();
        } else {
            error::_throw(error::Conflict);
        }
    }

    void KeyStore::setKV(Record& rec, ExclusiveTransaction &t) {
        setKV(rec.key(), rec.version(), rec.body(), t);
        rec.setExists();
    }

    bool KeyStore::createIndex(slice name,
                               slice expressionJSON,
                               IndexSpec::Type type,
                               const IndexSpec::Options* options)
    {
        return createIndex({string(name), type, alloc_slice(expressionJSON), options});
    }

    expiration_t KeyStore::now() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>
                (std::chrono::system_clock::now().time_since_epoch()).count();
    }

}
