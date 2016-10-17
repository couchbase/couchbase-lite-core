//
//  RecordEnumerator.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 6/18/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "RecordEnumerator.hh"
#include "KeyStore.hh"
#include "Logging.hh"
#include <algorithm>
#include <limits.h>
#include <string.h>

using namespace std;


namespace litecore {

    LogDomain EnumLog("Enum");

#pragma mark - ENUMERATION:


    RecordEnumerator::Options::Options()
    :skip(0),
     limit(UINT_MAX),
     descending(false),
     inclusiveStart(true),
     inclusiveEnd(true),
     includeDeleted(false),
     contentOptions(kDefaultContent)
    { }


    RecordEnumerator::RecordEnumerator(KeyStore &store, const Options& options)
    :_store(&store),
     _options(options)
    { }


    // Key-range constructor
    RecordEnumerator::RecordEnumerator(KeyStore &store,
                                 slice startKey, slice endKey,
                                 const Options& options)
    :RecordEnumerator(store, options)
    {
        LogToAt(EnumLog, Debug, "enum: RecordEnumerator(%s, [%s] -- [%s]%s) --> %p",
              store.name().c_str(), startKey.hexCString(), endKey.hexCString(),
              (options.descending ? " desc" : ""), this);
        if (startKey.size == 0)
            startKey.buf = nullptr;
        if (endKey.size == 0)
            endKey.buf = nullptr;

        slice minKey = startKey, maxKey = endKey;
        if (options.descending)
            swap(minKey, maxKey);

        _impl.reset(_store->newEnumeratorImpl(minKey, maxKey, _options));
        _skipStep = _impl->shouldSkipFirstStep();
    }

    // Sequence-range constructor
    RecordEnumerator::RecordEnumerator(KeyStore &store,
                                 sequence start, sequence end,
                                 const Options& options)
    :RecordEnumerator(store, options)
    {
        LogToAt(EnumLog, Debug, "enum: RecordEnumerator(%s, #%llu -- #%llu) --> %p",
                store.name().c_str(), start, end, this);

        sequence minSeq = start, maxSeq = end;
        if (options.descending)
            swap(minSeq, maxSeq);

        _impl.reset(_store->newEnumeratorImpl(minSeq, maxSeq, _options));
        _skipStep = _impl->shouldSkipFirstStep();
    }

    // Key-array constructor
    RecordEnumerator::RecordEnumerator(KeyStore &store,
                                 vector<string> recordIDs,
                                 const Options& options)
    :RecordEnumerator(store, options)
    {
        _recordIDs = recordIDs;
        LogToAt(EnumLog, Debug, "enum: RecordEnumerator(%s, %zu keys) --> %p",
                store.name().c_str(), recordIDs.size(), this);
        if (_options.skip > 0)
            _recordIDs.erase(_recordIDs.begin(), _recordIDs.begin() + _options.skip);
        if (_options.limit < _recordIDs.size())
            _recordIDs.resize(_options.limit);
        if (_options.descending)
            reverse(_recordIDs.begin(), _recordIDs.end());
        // (this mode doesn't actually create an fdb_iterator)
    }

    // Assignment from a temporary
    RecordEnumerator& RecordEnumerator::operator=(RecordEnumerator&& e) noexcept {
        _store = e._store;
        _impl = move(e._impl);
        _recordIDs = e._recordIDs;
        _curDocIndex = e._curDocIndex;
        _options = e._options;
        _skipStep = e._skipStep;
        return *this;
    }


    void RecordEnumerator::close() noexcept {
        _record.clear();
        _impl.reset();
    }


    bool RecordEnumerator::next() {
        // Enumerating an array of records is handled specially:
        if (_recordIDs.size() > 0)
            return nextFromArray();

        if (!_impl)
            return false;
        if (_options.limit-- == 0) {
            close();
            return false;
        }
        do {
            if (_skipStep) {
                _skipStep = false;
            } else if (!_impl->next()) {
                close();
                return false;
            }
        } while (_options.skip > 0 && _options.skip-- > 0);
        return getDoc();
    }

    // implementation of next() when enumerating a vector of keys
    bool RecordEnumerator::nextFromArray() {
        if (_curDocIndex >= _recordIDs.size()) {
            LogToAt(EnumLog, Debug, "enum: at end of vector");
            close();
            return false;
        }
        _record.clearMetaAndBody();
        _record.setKey(_recordIDs[_curDocIndex++]);
        _store->read(_record);
        LogToAt(EnumLog, Debug, "enum:     --> [%s]", _record.key().hexCString());
        return true;
    }

    bool RecordEnumerator::getDoc() {
        _record.clear();
        if (!_impl->read(_record)) {
            close();
            return false;
        }
        LogToAt(EnumLog, Debug, "enum:     --> [%s]", _record.key().hexCString());
        return true;
    }

}
