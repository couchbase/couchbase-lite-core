//
// RecordEnumerator.cc
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

#include "RecordEnumerator.hh"
#include "KeyStore.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <limits.h>
#include <string.h>

using namespace std;


namespace litecore {


    // By-key constructor
    RecordEnumerator::RecordEnumerator(KeyStore &store,
                                       Options options)
    :_store(&store)
    {
        LogVerbose(QueryLog, "RecordEnumerator %p: (%s, %d%d%d %d)",
                this, store.name().c_str(),
                options.includeDeleted, options.onlyConflicts, options.onlyBlobs,
                options.sortOption);
        _impl.reset(_store->newEnumeratorImpl(false, 0, options));
    }

    // By-sequence constructor
    RecordEnumerator::RecordEnumerator(KeyStore &store,
                                       sequence_t since,
                                       Options options)
    :_store(&store)
    {
        LogVerbose(QueryLog, "RecordEnumerator %p: (%s, #%llu..., %d%d%d %d)",
                this, store.name().c_str(), (unsigned long long)since,
                options.includeDeleted, options.onlyConflicts, options.onlyBlobs,
                options.sortOption);
        _impl.reset(_store->newEnumeratorImpl(true, since, options));
    }


    void RecordEnumerator::close() noexcept {
        _record.clear();
        _impl.reset();
    }


    bool RecordEnumerator::next() {
        if (!_impl) {
            return false;
        } else if (!_impl->next()) {
            close();
            return false;
        } else {
            _record.clear();
            if (!_impl->read(_record)) {
                close();
                return false;
            }
            LogDebug(QueryLog, "RecordEnumerator %p  --> '%.*s'", this, SPLAT(_record.key()));
            return true;
        }
    }

}
