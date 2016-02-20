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
#include <algorithm>
#include <limits.h>
#include <string.h>


namespace cbforest {

#pragma mark - ENUMERATION:


    const DocEnumerator::Options DocEnumerator::Options::kDefault = {
        0,
        UINT_MAX,
        false,
        true,
        true,
        false,
        KeyStore::kDefaultContent,
    };


    static fdb_iterator_opt_t iteratorOptions(const DocEnumerator::Options& options) {
        fdb_iterator_opt_t fdbOptions = 0;
        if (!options.includeDeleted) {
            // WORKAROUND for a ForestDB bug with reverse iteration [CBL#1082]
            if (!options.descending)
                fdbOptions |= FDB_ITR_NO_DELETES;
        }
        if (!options.inclusiveEnd)
            fdbOptions |= (options.descending ? FDB_ITR_SKIP_MIN_KEY : FDB_ITR_SKIP_MAX_KEY);
        if (!options.inclusiveStart)
            fdbOptions |= (options.descending ? FDB_ITR_SKIP_MAX_KEY : FDB_ITR_SKIP_MIN_KEY);
        return fdbOptions;
    }


    // Key-range constructor
    DocEnumerator::DocEnumerator(KeyStore store,
                                 slice startKey, slice endKey,
                                 const Options& options)
    :_store(store),
     _iterator(NULL),
     _options(options),
     _skipStep(true)
    {
        Debug("enum: DocEnumerator(%p, [%s] -- [%s]%s) --> %p",
              store.handle(),
              startKey.hexString().c_str(),
              endKey.hexString().c_str(),
              (options.descending ? " desc" : ""),
              this);
        if (startKey.size == 0)
            startKey.buf = NULL;
        if (endKey.size == 0)
            endKey.buf = NULL;

        slice minKey = startKey, maxKey = endKey;
        if (options.descending)
            std::swap(minKey, maxKey);

        fdb_status status = fdb_iterator_init(_store.handle(), &_iterator,
                                              minKey.buf, minKey.size,
                                              maxKey.buf, maxKey.size,
                                              iteratorOptions(options));
        check(status);
        initialPosition();
    }

    // Sequence-range constructor
    DocEnumerator::DocEnumerator(KeyStore store,
                                 sequence start, sequence end,
                                 const Options& options)
    :_store(store),
     _iterator(NULL),
     _options(options),
     _skipStep(true)
    {
        Debug("enum: DocEnumerator(%p, #%llu -- #%llu) --> %p",
                store.handle(), start, end, this);

        sequence minSeq = start, maxSeq = end;
        if (options.descending)
            std::swap(minSeq, maxSeq);

        check(fdb_iterator_sequence_init(store._handle, &_iterator,
                                         minSeq, maxSeq,
                                         iteratorOptions(options)));
        initialPosition();
    }

    void DocEnumerator::initialPosition() {
        if (_options.descending) {
            Debug("enum: fdb_iterator_seek_to_max(%p)", _iterator);
            fdb_iterator_seek_to_max(_iterator);  // ignore err; will fail if max key doesn't exist
        }
    }

    // Key-array constructor
    DocEnumerator::DocEnumerator(KeyStore handle,
                                 std::vector<std::string> docIDs,
                                 const Options& options)
    :_store(handle),
     _iterator(NULL),
     _options(options),
     _docIDs(docIDs),
     _curDocIndex(0)
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
    }

    // Empty constructor
    DocEnumerator::DocEnumerator()
    :_iterator(NULL)
    {
        Debug("enum: DocEnumerator() --> %p", this);
    }

    // Move constructor
    DocEnumerator::DocEnumerator(DocEnumerator&& e)
    :_store(e._store),
     _iterator(e._iterator),
     _options(e._options),
     _docIDs(e._docIDs),
     _curDocIndex(e._curDocIndex),
     _skipStep(e._skipStep)
    {
        Debug("enum: move ctor (from %p) --> %p", &e, this);
        e._iterator = NULL; // so e's destructor won't close the fdb_iterator
    }

    DocEnumerator::~DocEnumerator() {
        //Debug("enum: ~DocEnumerator(%p)", this);
        close();
    }

    // Assignment from a temporary
    DocEnumerator& DocEnumerator::operator=(DocEnumerator&& e) {
        Debug("enum: operator= %p <-- %p", this, &e);
        _store = e._store;
        _iterator = e._iterator;
        e._iterator = NULL; // so e's destructor won't close the fdb_iterator
        _docIDs = e._docIDs;
        _curDocIndex = e._curDocIndex;
        _options = e._options;
        _skipStep = e._skipStep;
        return *this;
    }


    void DocEnumerator::close() {
        freeDoc();
        if (_iterator) {
            Debug("enum: fdb_iterator_close(%p)", _iterator);
            fdb_iterator_close(_iterator);
            _iterator = NULL;
        }
    }


    bool DocEnumerator::next() {
        // Enumerating an array of docs is handled specially:
        if (_docIDs.size() > 0)
            return nextFromArray();

        if (!_iterator)
            return false;
        if (_options.limit-- == 0) {
            close();
            return false;
        }
        bool ignoreDeleted;
        do {
            if (_skipStep) {
                // The first time next() is called, don't advance the iterator
                _skipStep = false;
            } else {
                fdb_status status = _options.descending ? fdb_iterator_prev(_iterator)
                                                        : fdb_iterator_next(_iterator);
                Debug("enum: fdb_iterator_%s(%p) --> %d",
                      (_options.descending ?"prev" :"next"), _iterator, status);
                if (status == FDB_RESULT_ITERATOR_FAIL) {
                    close();
                    return false;
                }
                check(status);
            }
            
            // WORKAROUND for a ForestDB bug with reverse iteration [CBL#1082]
            ignoreDeleted = false;
            if (_options.descending && !_options.includeDeleted) {
                Document checkDoc;
                fdb_doc* docP = (fdb_doc*)checkDoc;
                if (fdb_iterator_get_metaonly(_iterator, &docP) == FDB_RESULT_SUCCESS
                        && checkDoc.deleted()) {
                    Debug("enum: ignoring deleted doc");
                    ignoreDeleted = true;
                }
            }
        } while (ignoreDeleted || (_options.skip > 0 && _options.skip-- > 0));
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
        fdb_status status;
        if (_options.contentOptions & KeyStore::kMetaOnly)
            status = fdb_get_metaonly(_store._handle, _doc);
        else
            status = fdb_get(_store._handle, _doc);
        if (status != FDB_RESULT_KEY_NOT_FOUND)
            check(status);
        Debug("enum:     fdb_get --> [%s]", _doc.key().hexString().c_str());
        return true;
    }

    void DocEnumerator::seek(slice key) {
        Debug("enum: seek([%s])", key.hexString().c_str());
        if (!_iterator)
            return;

        freeDoc();
        fdb_status status = fdb_iterator_seek(_iterator, key.buf, key.size,
                                              (_options.descending ? FDB_ITR_SEEK_LOWER
                                                                   : FDB_ITR_SEEK_HIGHER));
        if (status == FDB_RESULT_ITERATOR_FAIL) {
            close();
        } else {
            check(status);
            _skipStep = true; // so next() won't skip over the doc
        }
    }

    bool DocEnumerator::getDoc() {
        freeDoc();
        fdb_status status;
        fdb_doc* docP = (fdb_doc*)_doc;
        if (_options.contentOptions & KeyStore::kMetaOnly)
            status = fdb_iterator_get_metaonly(_iterator, &docP);
        else
            status = fdb_iterator_get(_iterator, &docP);
        CBFAssert(docP == (fdb_doc*)_doc);
        if (status == FDB_RESULT_ITERATOR_FAIL) {
            close();
            return false;
        }
        check(status);
        Debug("enum:     fdb_iterator_get --> [%s]", _doc.key().hexString().c_str());
        return true;
    }

    void DocEnumerator::freeDoc() {
        _doc.clearMetaAndBody();
        _doc.setKey(slice::null);
    }

}
