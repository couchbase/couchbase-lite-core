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

    const char* KeyStore::kIndexTypeName[] = {"value", "full-text", "array", "predictive"};


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

    void KeyStore::get(sequence_t seq, function_ref<void(const Record&)> fn) {
        fn(get(seq));
    }

    void KeyStore::readBody(Record &rec) const {
        if (!rec.body()) {
            Record fullDoc = rec.sequence() ? get(rec.sequence())
                                            : get(rec.key(), kEntireBody);
            rec._body = fullDoc._body;
        }
    }

#if ENABLE_DELETE_KEY_STORES
    void KeyStore::deleteKeyStore(Transaction& trans) {
        trans.dataFile().deleteKeyStore(name());
    }
#endif
    
    void KeyStore::write(Record &rec, Transaction &t, const sequence_t *replacingSequence) {
        auto seq = set(rec.key(), rec.version(), rec.body(), rec.flags(), t, replacingSequence);
        rec.setExists();
        rec.updateSequence(seq);
    }

    bool KeyStore::setDocumentFlag(slice key, sequence_t sequence, DocumentFlags, Transaction&) {
        error::_throw(error::Unimplemented);
    }

    bool KeyStore::createIndex(const IndexSpec&, const IndexOptions*) {
        error::_throw(error::Unimplemented);
    }

    bool KeyStore::createIndex(slice name,
                               slice expressionJSON,
                               IndexType type,
                               const IndexOptions* options)
    {
        return createIndex({string(name), type, alloc_slice(expressionJSON)}, options);
    }


    void KeyStore::deleteIndex(slice name) {
        error::_throw(error::Unimplemented);
    }
    
    vector<KeyStore::IndexSpec> KeyStore::getIndexes() const {
        error::_throw(error::Unimplemented);
    }

    Retained<Query> KeyStore::compileQuery(slice expressionJSON) {
        error::_throw(error::Unimplemented);
    }

    expiration_t KeyStore::now() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>
                (std::chrono::system_clock::now().time_since_epoch()).count();
    }

}
