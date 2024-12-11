//
// RecordEnumerator.cc
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "RecordEnumerator.hh"
#include "KeyStore.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <climits>
#include <cstring>

using namespace std;

namespace litecore {


    // By-key constructor
    RecordEnumerator::RecordEnumerator(KeyStore& store, Options const& options) : _store(&store) {
        if ( options.minSequence != 0_seq ) {
            LogVerbose(QueryLog, "RecordEnumerator %p: (%s, #%llu..., %d%d%d %d)", this, store.name().c_str(),
                       (unsigned long long)options.minSequence, options.includeDeleted, options.onlyConflicts,
                       options.onlyBlobs, options.sortOption);
        } else {
            LogVerbose(QueryLog, "RecordEnumerator %p: (%s, %d%d%d %d)", this, store.name().c_str(),
                       options.includeDeleted, options.onlyConflicts, options.onlyBlobs, options.sortOption);
        }
        _impl.reset(_store->newEnumeratorImpl(options));
    }

    void RecordEnumerator::close() noexcept {
        _record.clear();
        _impl.reset();
    }

    bool RecordEnumerator::next() {
        if ( !_impl ) {
            return false;
        } else if ( !_impl->next() ) {
            close();
            return false;
        } else {
            _record.clear();
            if ( !_impl->read(_record) ) {
                close();
                return false;
            }
            LogDebug(QueryLog, "RecordEnumerator %p  --> '%.*s'", this, SPLAT(_record.key()));
            return true;
        }
    }

}  // namespace litecore
