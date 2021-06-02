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

    const KeyStore::Capabilities KeyStore::Capabilities::defaults = {false};


    Record KeyStore::get(slice key, ContentOption option) const {
        Record rec(key);
        read(rec, option);
        return rec;
    }

    void KeyStore::get(slice key, ContentOption option, function_ref<void(const Record&)> fn) {
        // Subclasses can implement this differently for better memory management.
        Record rec(key);
        read(rec, option);
        fn(rec);
    }

#if ENABLE_DELETE_KEY_STORES
    void KeyStore::deleteKeyStore(Transaction& trans) {
        trans.dataFile().deleteKeyStore(name());
    }
#endif

    sequence_t KeyStore::set(Record &rec,
                             Transaction &t,
                             optional<sequence_t> replacingSequence,
                             bool newSequence)
    {
        RecordLite r = {rec.key(), rec.version(), rec.body(), nullslice,
                          replacingSequence, newSequence, rec.flags()};
        auto seq = set(r, t);
        if (seq > 0) {
            rec.setExists();
            rec.updateSequence(seq);
        }
        return seq;
    }

    bool KeyStore::createIndex(slice name,
                               slice expression,
                               QueryLanguage queryLanguage,
                               IndexSpec::Type type,
                               const IndexSpec::Options* options)
    {
        return createIndex({string(name), type, alloc_slice(expression), queryLanguage, options});
    }

    expiration_t KeyStore::now() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>
                (std::chrono::system_clock::now().time_since_epoch()).count();
    }

}
