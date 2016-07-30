//
//  DocEnumerator.cc
//  CBNano
//
//  Created by Jens Alfke on 6/18/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "DocEnumerator.hh"
#include "KeyStore.hh"
#include "LogInternal.hh"
#include <algorithm>
#include <limits.h>
#include <string.h>

using namespace std;


namespace cbforest {

#pragma mark - ENUMERATION:


    const DocEnumerator::Options DocEnumerator::Options::kDefault = {
        0,
        UINT_MAX,
        false,
        true,
        true,
        false,
        kDefaultContent,
    };


    DocEnumerator::DocEnumerator(KeyStore &store, const Options& options)
    :_store(&store),
     _options(options)
    { }


    // Key-range constructor
    DocEnumerator::DocEnumerator(KeyStore &store,
                                 slice startKey, slice endKey,
                                 const Options& options)
    :DocEnumerator(store, options)
    {
        Debug("enum: DocEnumerator(%s, [%s] -- [%s]%s) --> %p",
              store.name().c_str(), startKey.hexCString(), endKey.hexCString(),
              (options.descending ? " desc" : ""), this);
        if (startKey.size == 0)
            startKey.buf = NULL;
        if (endKey.size == 0)
            endKey.buf = NULL;

        slice minKey = startKey, maxKey = endKey;
        if (options.descending)
            swap(minKey, maxKey);

        _impl.reset(_store->newEnumeratorImpl(minKey, maxKey, _options));
        _skipStep = _impl->shouldSkipFirstStep();
    }

    // Sequence-range constructor
    DocEnumerator::DocEnumerator(KeyStore &store,
                                 sequence start, sequence end,
                                 const Options& options)
    :DocEnumerator(store, options)
    {
        Debug("enum: DocEnumerator(%s, #%llu -- #%llu) --> %p",
                store.name().c_str(), start, end, this);

        sequence minSeq = start, maxSeq = end;
        if (options.descending)
            swap(minSeq, maxSeq);

        _impl.reset(_store->newEnumeratorImpl(minSeq, maxSeq, _options));
        _skipStep = _impl->shouldSkipFirstStep();
    }

    // Key-array constructor
    DocEnumerator::DocEnumerator(KeyStore &store,
                                 vector<string> docIDs,
                                 const Options& options)
    :DocEnumerator(store, options)
    {
        _docIDs = docIDs;
        Debug("enum: DocEnumerator(%s, %zu keys) --> %p",
                store.name().c_str(), docIDs.size(), this);
        if (_options.skip > 0)
            _docIDs.erase(_docIDs.begin(), _docIDs.begin() + _options.skip);
        if (_options.limit < _docIDs.size())
            _docIDs.resize(_options.limit);
        if (_options.descending)
            reverse(_docIDs.begin(), _docIDs.end());
        // (this mode doesn't actually create an fdb_iterator)
    }

    DocEnumerator::~DocEnumerator() {
        //Debug("enum: ~DocEnumerator(%p)", this);
        close();
    }

    // Assignment from a temporary
    DocEnumerator& DocEnumerator::operator=(DocEnumerator&& e) {
        Debug("enum: operator= %p <-- %p", this, &e);
        _store = e._store;
        _impl = move(e._impl);
        e._impl = nullptr;
        _docIDs = e._docIDs;
        _curDocIndex = e._curDocIndex;
        _options = e._options;
        _skipStep = e._skipStep;
        return *this;
    }


    void DocEnumerator::close() {
        _doc.clear();
        _impl.reset();
    }


    bool DocEnumerator::next() {
        // Enumerating an array of docs is handled specially:
        if (_docIDs.size() > 0)
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
    bool DocEnumerator::nextFromArray() {
        if (_curDocIndex >= _docIDs.size()) {
            Debug("enum: at end of vector");
            close();
            return false;
        }
        _doc.clearMetaAndBody();
        _doc.setKey(_docIDs[_curDocIndex++]);
        _store->read(_doc);
        Debug("enum:     fdb_get --> [%s]", _doc.key().hexCString());
        return true;
    }

    void DocEnumerator::seek(slice key) {
        Debug("enum: seek([%s])", key.hexCString());
        if (!_impl)
            return;

        _doc.clear();
        if (_impl->seek(key))
            _skipStep = true; // so next() won't skip over the doc
        else
            close();
    }

    bool DocEnumerator::getDoc() {
        _doc.clear();
        if (!_impl->read(_doc)) {
            close();
            return false;
        }
        Debug("enum:     fdb_iterator_get --> [%s]", _doc.key().hexCString());
        return true;
    }

}
