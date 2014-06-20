//
//  DocEnumerator.cc
//  CBForest
//
//  Created by Jens Alfke on 6/18/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "DocEnumerator.hh"
#include "forestdb.h"

namespace forestdb {

#pragma mark - ENUMERATION:


    static void check(fdb_status status) {
        if (status != FDB_RESULT_SUCCESS)
            throw error{status};
    }


    const DocEnumerator::enumerationOptions DocEnumerator::enumerationOptions::kDefault = {
        .skip = 0,
        .limit = UINT_MAX,
//        .descending = false,
        .inclusiveEnd = true,
        .includeDeleted = false,
        .onlyConflicts = false,
        .contentOptions = Database::kDefaultContent,
    };


    static fdb_iterator_opt_t iteratorOptions(const DocEnumerator::enumerationOptions& options) {
        fdb_iterator_opt_t fdbOptions = 0;
        if (options.contentOptions & DatabaseGetters::kMetaOnly)
            fdbOptions |= FDB_ITR_METAONLY;
        if (!options.includeDeleted)
            fdbOptions |= FDB_ITR_NO_DELETES;
        return fdbOptions;
    }
    
    
    DocEnumerator::DocEnumerator(DatabaseGetters* db,
                                 slice startKey, slice endKey,
                                 const enumerationOptions& options)
    :_db(db),
     _iterator(NULL),
     _endKey(endKey),
     _options(options),
     _docP(NULL)
    {
        fprintf(stderr, "enum: DocEnumerator(%p, [%s] -- [%s]) --> %p\n",
                db,
                startKey.hexString().c_str(),
                endKey.hexString().c_str(), this);
        if (startKey.size == 0)
            startKey.buf = NULL;
        if (endKey.size == 0)
            endKey.buf = NULL;
        restartFrom(startKey, endKey);
        next();
    }

    DocEnumerator::DocEnumerator(DatabaseGetters* db,
                                 sequence start, sequence end,
                                 const enumerationOptions& options)
    :_db(db),
     _iterator(NULL),
     _endKey(),
     _options(options),
     _docP(NULL)
    {
        fprintf(stderr, "enum: DocEnumerator(%p, #%llu -- #%llu) --> %p\n",
                db, start, end, this);
        check(fdb_iterator_sequence_init(db->_handle, &_iterator,
                                         start, end,
                                         iteratorOptions(options)));
        next();
    }

    DocEnumerator::DocEnumerator(DatabaseGetters* db,
                                 std::vector<std::string> docIDs,
                                 const enumerationOptions& options)
    :_db(db),
     _iterator(NULL),
     _endKey(),
     _options(options),
     _docIDs(docIDs),
     _curDocIndex(-1),
     _docP(NULL)
    {
        fprintf(stderr, "enum: DocEnumerator(%p, %zu keys) --> %p\n",
                db, docIDs.size(), this);
        if (docIDs.size() == 0)
            return;
        restartFrom(docIDs[0]);
        next();
    }


    DocEnumerator::DocEnumerator()
    :_iterator(NULL),
     _docP(NULL)
    {
        fprintf(stderr, "enum: DocEnumerator() --> %p\n", this);
    }

    DocEnumerator::DocEnumerator(DocEnumerator&& e)
    :_db(e._db),
     _iterator(e._iterator),
     _endKey(e._endKey),
     _options(e._options),
     _docIDs(e._docIDs),
     _curDocIndex(e._curDocIndex),
     _docP(e._docP)
    {
        fprintf(stderr, "enum: move ctor (from %p) --> %p\n", &e, this);
        e._iterator = NULL; // so e's destructor won't close the fdb_iterator
        e._docP = NULL;
    }

    DocEnumerator::~DocEnumerator() {
        fprintf(stderr, "enum: ~DocEnumerator(%p)\n", this);
        close();
    }

    DocEnumerator& DocEnumerator::operator=(DocEnumerator&& e) {
        fprintf(stderr, "enum: operator= %p <-- %p\n", &e, this);
        _db = e._db;
        _iterator = e._iterator;
        e._iterator = NULL; // so e's destructor won't close the fdb_iterator
        _endKey = e._endKey;
        _docIDs = e._docIDs;
        _curDocIndex = e._curDocIndex;
        _options = e._options;
        _docP = e._docP;
        e._docP = NULL;
        return *this;
    }


    void DocEnumerator::restartFrom(slice startKey, slice endKey) {
        if (_iterator)
            fdb_iterator_close(_iterator);
        check(fdb_iterator_init(_db->_handle, &_iterator,
                                startKey.buf, startKey.size,
                                endKey.buf, endKey.size,
                                iteratorOptions(_options)));
    }


    void DocEnumerator::close() {
        fprintf(stderr, "enum: close %p (free %p, close %p)\n", this, _docP, _iterator);
        freeDoc();
        if (_iterator) {
            fdb_iterator_close(_iterator);
            _iterator = NULL;
        }
    }


    bool DocEnumerator::next() {
        if (!_iterator)
            return false;

        fdb_status status;
        if (_docIDs.size() == 0) {
            // Regular iteration:
            freeDoc();
            status = fdb_iterator_next(_iterator, &_docP);
            fprintf(stderr, "enum: fdb_iterator_next --> %d\n", status);
            if (status == FDB_RESULT_ITERATOR_FAIL) {
                close();
                return false;
            }
            check(status);
            if (!_options.inclusiveEnd && doc().key().equal(_endKey)) {
                close();
                return false;
            }
        } else {
            // Iterating over a vector of docIDs:
           if (++_curDocIndex >= _docIDs.size()) {
                fprintf(stderr, "enum: at end of vector\n");
                close();
                return false;
            }
            slice docID = _docIDs[_curDocIndex];
            if (!seek(docID) || !slice(_docP->key, _docP->keylen).equal(docID)) {
                // If the current doc doesn't match the docID, then the docID doesn't exist:
                fdb_doc_free(_docP);
                fdb_doc_create(&_docP, docID.buf, docID.size, NULL, 0, NULL, 0);
            }
        }
        return true;
    }

    bool DocEnumerator::seek(slice key) {
        fprintf(stderr, "enum: seek([%s])\n", key.hexString().c_str());
        if (!_iterator)
            return false;

        bool forwards = _docP == NULL || doc().key() < key;
        freeDoc();

        if (forwards) {
            check(fdb_iterator_seek(_iterator, key.buf, key.size));
        } else {
            // ForestDB can't seek backwards [MB-11470] so I have to restart the iterator:
            restartFrom(key);
        }

        fdb_status status = fdb_iterator_next(_iterator, &_docP);
        if (status == FDB_RESULT_ITERATOR_FAIL)
            return false;
        check(status);
        return true;
    }

}
