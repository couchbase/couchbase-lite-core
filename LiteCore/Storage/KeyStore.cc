//
// KeyStore.cc
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "KeyStore.hh"
#include "Query.hh"
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

    Retained<Query> KeyStore::compileQuery(slice expr, QueryLanguage language) {
        return dataFile().compileQuery(expr, language, this);
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
