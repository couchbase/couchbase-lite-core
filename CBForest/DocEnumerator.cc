//
//  DocEnumerator.cc
//  CBForest
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
#include "LogInternal.hh"
#include "forestdb.h"
#include <assert.h>
#include <string.h>


namespace forestdb {

#pragma mark - ENUMERATION:


    static void check(fdb_status status) {
        if (status != FDB_RESULT_SUCCESS)
            throw error{status};
    }


    const DocEnumerator::Options DocEnumerator::Options::kDefault = {
        .skip = 0,
        .limit = UINT_MAX,
        .descending = false,
        .inclusiveStart = true,
        .inclusiveEnd = true,
        .includeDeleted = false,
        .contentOptions = KeyStore::kDefaultContent,
    };


    static fdb_iterator_opt_t iteratorOptions(const DocEnumerator::Options& options) {
        fdb_iterator_opt_t fdbOptions = 0;
        if (options.contentOptions & KeyStore::kMetaOnly)
            fdbOptions |= FDB_ITR_METAONLY;
        if (!options.includeDeleted)
            fdbOptions |= FDB_ITR_NO_DELETES;
        return fdbOptions;
    }
    

    // Key-range constructor
    DocEnumerator::DocEnumerator(KeyStore store,
                                 slice startKey, slice endKey,
                                 const Options& options)
    :_store(store),
     _iterator(NULL),
     _startKey(startKey),
     _endKey(endKey),
     _options(options),
     _docP(NULL),
     _skipStep(false)
    {
        Debug("enum: DocEnumerator(%p, [%s] -- [%s]%s) --> %p",
              store.handle(),
              _startKey.hexString().c_str(),
              _endKey.hexString().c_str(),
              (options.descending ? " desc" : ""),
              this);
        if (_startKey.size == 0)
            _startKey.buf = NULL;
        if (_endKey.size == 0)
            _endKey.buf = NULL;

        start();
        next();
    }

    // Sequence-range constructor
    DocEnumerator::DocEnumerator(KeyStore store,
                                 sequence start, sequence end,
                                 const Options& options)
    :_store(store),
     _iterator(NULL),
     _startKey(),
     _endKey(),
     _options(options),
     _docP(NULL),
     _skipStep(false)
    {
        Debug("enum: DocEnumerator(%p, #%llu -- #%llu) --> %p",
                store.handle(), start, end, this);
        check(fdb_iterator_sequence_init(store._handle, &_iterator,
                                         start, end,
                                         iteratorOptions(options)));
        next();
    }

    // Key-array constructor
    DocEnumerator::DocEnumerator(KeyStore handle,
                                 std::vector<std::string> docIDs,
                                 const Options& options)
    :_store(handle),
     _iterator(NULL),
     _startKey(),
     _endKey(),
     _options(options),
     _docIDs(docIDs),
     _curDocIndex(0),
     _docP(NULL),
     _skipStep(false)
    {
        Debug("enum: DocEnumerator(%p, %zu keys) --> %p",
                handle, docIDs.size(), this);
        if (_options.skip > 0)
            _docIDs.erase(_docIDs.begin(), _docIDs.begin() + _options.skip);
        if (_options.limit < _docIDs.size())
            _docIDs.resize(_options.limit);
        if (_options.descending)
            std::reverse(_docIDs.begin(), _docIDs.end());
        // (this mode doesn't actually create an fdb_iterator)
        nextFromArray();
    }


    DocEnumerator::DocEnumerator()
    :_iterator(NULL),
     _docP(NULL)
    {
        Debug("enum: DocEnumerator() --> %p", this);
    }

    DocEnumerator::DocEnumerator(DocEnumerator&& e)
    :_store(e._store),
     _iterator(e._iterator),
     _startKey(e._startKey),
     _endKey(e._endKey),
     _options(e._options),
     _docIDs(e._docIDs),
     _curDocIndex(e._curDocIndex),
     _docP(e._docP),
     _skipStep(e._skipStep)
    {
        Debug("enum: move ctor (from %p) --> %p", &e, this);
        e._iterator = NULL; // so e's destructor won't close the fdb_iterator
        e._docP = NULL;
    }

    DocEnumerator::~DocEnumerator() {
        Debug("enum: ~DocEnumerator(%p)", this);
        close();
    }

    DocEnumerator& DocEnumerator::operator=(DocEnumerator&& e) {
        Debug("enum: operator= %p <-- %p", this, &e);
        _store = e._store;
        _iterator = e._iterator;
        e._iterator = NULL; // so e's destructor won't close the fdb_iterator
        _startKey = e._startKey;
        _endKey = e._endKey;
        _docIDs = e._docIDs;
        _curDocIndex = e._curDocIndex;
        _options = e._options;
        _docP = e._docP;
        e._docP = NULL;
        _skipStep = e._skipStep;
        return *this;
    }


    // common subroutine of the key-range and key-array constructors
    void DocEnumerator::start() {
        slice minKey = _startKey, maxKey = _endKey;
        if (_options.descending)
            std::swap(minKey, maxKey);
        if (_iterator)
            fdb_iterator_close(_iterator);
        check(fdb_iterator_init(_store.handle(), &_iterator,
                                minKey.buf, minKey.size,
                                maxKey.buf, maxKey.size,
                                iteratorOptions(_options)));
        if (_options.descending) {
            if (_startKey.size)
                fdb_iterator_seek(_iterator, _startKey.buf, _startKey.size);
            else
                fdb_iterator_seek_to_max(_iterator);
            _skipStep = true;
        }
    }


    void DocEnumerator::close() {
        Debug("enum: close %p (free %p, close %p)", this, _docP, _iterator);
        freeDoc();
        if (_iterator) {
            fdb_iterator_close(_iterator);
            _iterator = NULL;
        }
    }


    bool DocEnumerator::next() {
        // Enumerating an array of docs is handled specially:
        if (_docIDs.size() > 0)
            return nextFromArray();

        while(true) {
            if (!_iterator)
                return false;

            freeDoc();
            if (_skipStep) {
                // The first time next() is called, don't need to advance the iterator
                _skipStep = false;
            } else {
                fdb_status status = _options.descending ? fdb_iterator_prev(_iterator)
                                                        : fdb_iterator_next(_iterator);
                Debug("enum: fdb_iterator_next(%p) --> %d", _iterator, status);
                if (status == FDB_RESULT_ITERATOR_FAIL) {
                    close();
                    return false;
                }
                check(status);
            }

            getDoc();

            if (!_options.inclusiveEnd && doc().key() == _endKey) {
                close();
                return false;
            }
            if (!_options.inclusiveStart && doc().key() == _startKey) {
                continue;
            }

            // OK, this is a candidate. First honor the skip and limit:
            if (_options.skip > 0) {
                --_options.skip;
                continue;
            }
            if (_options.limit-- == 0) {
                close();
                return false;
            }
            return true;
        }
    }

    // implementation of next() when enumerating a vector of keys
    bool DocEnumerator::nextFromArray() {
        if (_curDocIndex >= _docIDs.size()) {
            Debug("enum: at end of vector");
            close();
            return false;
        }
        freeDoc();
        slice docID = _docIDs[_curDocIndex++];
        fdb_doc_create(&_docP, docID.buf, docID.size, NULL, 0, NULL, 0);
        fdb_status status;
        if (_options.contentOptions & KeyStore::kMetaOnly)
            status = fdb_get_metaonly(_store._handle, _docP);
        else
            status = fdb_get(_store._handle, _docP);
        if (status != FDB_RESULT_KEY_NOT_FOUND)
            check(status);
        Debug("enum:     fdb_get --> [%s]", slice(_docP->key, _docP->keylen).hexString().c_str());
        return true;
    }

    bool DocEnumerator::seek(slice key) {
        Debug("enum: seek([%s])", key.hexString().c_str());
        if (!_iterator)
            return false;

        freeDoc();
        fdb_status status = fdb_iterator_seek(_iterator, key.buf, key.size);
        if (status == FDB_RESULT_ITERATOR_FAIL)
            return false;
        check(status);
        getDoc();
        return true;
    }

    void DocEnumerator::getDoc() {
        assert(_docP == NULL);
        fdb_status status;
        if (_options.contentOptions & KeyStore::kMetaOnly)
            status = fdb_iterator_get_metaonly(_iterator, &_docP);
        else
            status = fdb_iterator_get(_iterator, &_docP);
        check(status);
        Debug("enum:     fdb_iterator_get --> [%s]", slice(_docP->key, _docP->keylen).hexString().c_str());
    }

}
