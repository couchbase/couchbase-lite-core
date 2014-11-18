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
        .onlyConflicts = false,
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
    
    
    DocEnumerator::DocEnumerator(KeyStore store,
                                 slice startKey, slice endKey,
                                 const Options& options)
    :_store(store),
     _iterator(NULL),
     _startKey(startKey),
     _endKey(endKey),
     _options(options),
     _docP(NULL)
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

        // HACK to work around limitations of fdb_iterator_seek until MB-12465 is fixed
        char maxKeyBuf[FDB_MAX_KEYLEN];
        slice firstKey;
        if (_options.descending) {
            if (_startKey.size > 0) {
                firstKey = _startKey;
            } else {
                memset(maxKeyBuf, 0xFF, sizeof(maxKeyBuf));
                firstKey = slice(maxKeyBuf, sizeof(maxKeyBuf));
            }
        }
        restartFrom(firstKey);
        if (_options.descending && _startKey.size == 0)
            _firstDescending = false; //HACK MB-12465
        next();
    }

    DocEnumerator::DocEnumerator(KeyStore store,
                                 sequence start, sequence end,
                                 const Options& options)
    :_store(store),
     _iterator(NULL),
     _startKey(),
     _endKey(),
     _options(options),
     _docP(NULL)
    {
        Debug("enum: DocEnumerator(%p, #%llu -- #%llu) --> %p",
                store.handle(), start, end, this);
        check(fdb_iterator_sequence_init(store._handle, &_iterator,
                                         start, end,
                                         iteratorOptions(options)));
        next();
    }

    DocEnumerator::DocEnumerator(KeyStore handle,
                                 std::vector<std::string> docIDs,
                                 const Options& options)
    :_store(handle),
     _iterator(NULL),
     _startKey(),
     _endKey(),
     _options(options),
     _docIDs(docIDs),
     _curDocIndex(-1),
     _docP(NULL)
    {
        Debug("enum: DocEnumerator(%p, %zu keys) --> %p",
                handle, docIDs.size(), this);
        if (docIDs.size() == 0)
            return;
        if (_options.descending) {
            std::reverse(_docIDs.begin(), _docIDs.end());
            _options.descending = false;
        }
        restartFrom(docIDs[0]);
        next();
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
     _firstDescending(e._firstDescending)
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
        _firstDescending = e._firstDescending;
        _docP = e._docP;
        e._docP = NULL;
        return *this;
    }


    void DocEnumerator::restartFrom(slice firstKey) {
        slice minKey = _startKey, maxKey = _endKey;
        _firstDescending = _options.descending; //HACK MB-12465
        if (_options.descending)
            std::swap(minKey, maxKey);
        if (_iterator)
            fdb_iterator_close(_iterator);
        check(fdb_iterator_init(_store.handle(), &_iterator,
                                minKey.buf, minKey.size,
                                maxKey.buf, maxKey.size,
                                iteratorOptions(_options)));
        if (firstKey.buf) {
            Debug("enum: seek(%s)", firstKey.hexString().c_str());
            fdb_iterator_seek(_iterator, firstKey.buf, firstKey.size);
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
        while(true) {
            if (!_iterator)
                return false;

            fdb_status status;
            if (_docIDs.size() == 0) {
                // Regular iteration:
                freeDoc();
                if (_options.descending && !_firstDescending) {
                    status = fdb_iterator_prev(_iterator, &_docP);
                    Debug("enum: fdb_iterator_prev(%p) --> %d", _iterator, status);
                } else {
                    status = fdb_iterator_next(_iterator, &_docP);
                    Debug("enum: fdb_iterator_next(%p) --> %d", _iterator, status);
                }
                _firstDescending = false; //HACK MB-12465
                if (status == FDB_RESULT_ITERATOR_FAIL) {
                    close();
                    return false;
                }
                check(status);
                if (!_options.inclusiveEnd && doc().key() == _endKey) {
                    close();
                    return false;
                }
                if (!_options.inclusiveStart && doc().key() == _startKey) {
                    continue;
                }
            } else {
                // Iterating over a vector of docIDs:
               if (++_curDocIndex >= _docIDs.size()) {
                    Debug("enum: at end of vector");
                    close();
                    return false;
                }
                slice docID = _docIDs[_curDocIndex];
                if (!seek(docID) || slice(_docP->key, _docP->keylen) != docID) {
                    // If the current doc doesn't match the docID, then the docID doesn't exist:
                    fdb_doc_free(_docP);
                    fdb_doc_create(&_docP, docID.buf, docID.size, NULL, 0, NULL, 0);
                }
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

    bool DocEnumerator::seek(slice key) {
        Debug("enum: seek([%s])", key.hexString().c_str());
        if (!_iterator)
            return false;

        freeDoc();
        check(fdb_iterator_seek(_iterator, key.buf, key.size));
        fdb_status status = fdb_iterator_next(_iterator, &_docP);
        if (status == FDB_RESULT_ITERATOR_FAIL)
            return false;
        check(status);
        return true;
    }

}
