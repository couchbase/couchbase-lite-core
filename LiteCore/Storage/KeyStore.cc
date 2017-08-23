//
//  KeyStore.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/12/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "KeyStore.hh"
#include "Record.hh"
#include "DataFile.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Logging.hh"

using namespace std;


namespace litecore {

    extern LogDomain DBLog;

    const KeyStore::Capabilities KeyStore::Capabilities::defaults = {false};

    Record KeyStore::get(slice key, ContentOptions options) const {
        Record rec(key);
        read(rec, options);
        return rec;
    }

    void KeyStore::get(slice key, ContentOptions options, function_ref<void(const Record&)> fn) {
        // Subclasses can implement this differently for better memory management.
        Record rec(key);
        read(rec, options);
        fn(rec);
    }

    void KeyStore::get(sequence_t seq, ContentOptions options, function_ref<void(const Record&)> fn) {
        fn(get(seq, options));
    }

    void KeyStore::readBody(Record &rec) const {
        if (!rec.body()) {
            Record fullDoc = rec.sequence() ? get(rec.sequence(), kDefaultContent)
                                              : get(rec.key(), kDefaultContent);
            rec._body = fullDoc._body;
        }
    }

    void KeyStore::deleteKeyStore(Transaction& trans) {
        trans.dataFile().deleteKeyStore(name());
    }

    void KeyStore::write(Record &rec, Transaction &t, const sequence_t *replacingSequence) {
        auto seq = set(rec.key(), rec.version(), rec.body(), rec.flags(), t, replacingSequence);
        rec.setExists();
        rec.updateSequence(seq);
    }

    bool KeyStore::del(slice key, sequence_t seq, Transaction &t) {
        if (key) {
            LogVerbose(DBLog, "KeyStore(%s) del key '%.*s'", _name.c_str(), SPLAT(key));
        } else {
            LogVerbose(DBLog, "KeyStore(%s) del seq %llu", _name.c_str(), (unsigned long long)seq);
        }
        
        if (!_del(key, seq, t))
            return false;
        t.incrementPurgeCount();
        return true;
    }

    bool KeyStore::del(const litecore::Record &rec, Transaction &t) {
        return del(rec.key(), 0, t);
    }

    bool KeyStore::setDocumentFlag(slice key, sequence_t sequence, DocumentFlags) {
        error::_throw(error::Unimplemented);
    }

    void KeyStore::createIndex(slice name, slice expressionJSON, IndexType, const IndexOptions*) {
        error::_throw(error::Unimplemented);
    }

    void KeyStore::deleteIndex(slice name) {
        error::_throw(error::Unimplemented);
    }
    
    alloc_slice KeyStore::getIndexes() const {
        error::_throw(error::Unimplemented);
    }

    Retained<Query> KeyStore::compileQuery(slice expressionJSON) {
        error::_throw(error::Unimplemented);
    }

}
