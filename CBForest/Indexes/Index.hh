//
//  Index.hh
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__Index__
#define __CBForest__Index__

#include "DataFile.hh"
#include "DocEnumerator.hh"
#include "Collatable.hh"
#include <atomic>

namespace cbforest {
    
    class Index;


    /** Struct representing range of Index keys. */
    struct KeyRange {
        Collatable start;
        Collatable end;
        bool inclusiveEnd;

        KeyRange(Collatable s, Collatable e, bool inclusive =true)
                                                :start(s), end(e), inclusiveEnd(inclusive) { }
        KeyRange(Collatable single)             :start(single), end(single), inclusiveEnd(true) { }
        KeyRange(const KeyRange &r)             :start(r.start), end(r.end),
                                                 inclusiveEnd(r.inclusiveEnd) { }
        bool isKeyPastEnd(slice key) const;

        bool operator== (const KeyRange &r)     {return start==r.start && end==r.end;}
    };

    
    /** A key/value index, stored in a KeyStore. */
    class Index {
    public:
        explicit Index(KeyStore&);
        ~Index();

        alloc_slice getEntry(slice docID, sequence docSequence,
                             Collatable key,
                             unsigned emitIndex) const;

        DataFile& dataFile() const              {return _store.dataFile();}
        bool isBusy() const                     {return _userCount > 0;}

        /** Used as a placeholder for an index value that's stored out of line, i.e. that
            represents the entire document being indexed. */
        static const slice kSpecialValue;

    protected:
        KeyStore &_store;

    private:
        friend class IndexWriter;
        friend class IndexEnumerator;

        void addUser()                          {++_userCount;}
        void removeUser()                       {--_userCount;}

        std::atomic_uint _userCount {0};
    };


    /** Updates an index, within a Transaction. */
    class IndexWriter {
    public:
        IndexWriter(Index& index, Transaction& t);
        ~IndexWriter();

        /** Updates the index entry for a document with the given keys and values.
            Adjusts the value of rowCount by the number of rows added or removed.
            Returns true if the index may have changed as a result. */
        bool update(slice docID,
                    sequence docSequence,
                    const std::vector<Collatable> &keys,
                    const std::vector<alloc_slice> &values,
                    uint64_t &rowCount);

    private:
        void getKeysForDoc(slice docID, std::vector<Collatable> &outKeys, uint32_t &outHash);
        void setKeysForDoc(slice docID, const std::vector<Collatable> &keys, uint32_t hash);

        friend class Index;
        friend class MapReduceIndex;

        Index &         _index;             // The Index being written to
        Transaction &   _transaction;       // The Transaction enabling the write
    };


    /** Index query enumerator. */
    class IndexEnumerator {
    public:
        IndexEnumerator(Index&,
                        Collatable startKey, slice startKeyDocID,
                        Collatable endKey, slice endKeyDocID,
                        const DocEnumerator::Options&);

        IndexEnumerator(Index&,
                        std::vector<KeyRange> keyRanges,
                        const DocEnumerator::Options&);

        virtual ~IndexEnumerator()              {_index.removeUser();}

        const Index& index() const              {return _index;}

        CollatableReader key() const            {return CollatableReader(_key);}
        slice value() const                     {return _value;}
        slice docID() const                     {return _docID;}
        sequence_t sequence() const             {return _sequence;}

        int currentKeyRangeIndex()              {return _currentKeyIndex;}

        bool next();

        void close()                            {_dbEnum.close();}

    protected:
        bool nextKeyRange();
        virtual bool approve(slice key)         {return true;}
        bool read();
        void setValue(slice value)              {_value = value;}
        DocEnumerator enumeratorForIndex(int keyRangeIndex);

    private:
        friend class Index;

        Index&                  _index;                 // The index
        DocEnumerator::Options  _options;               // Enumeration options
        alloc_slice             _startKey;              // Key to start at
        alloc_slice             _endKey;                // Key to end at
        bool                    _inclusiveStart;        // Include the startKey?
        bool                    _inclusiveEnd;          // Include the endKey?
        std::vector<KeyRange>   _keyRanges;             // Ranges of keys to traverse (optional)
        int                     _currentKeyIndex {-1};  // Current key range's index or -1

        DocEnumerator           _dbEnum;                // The underlying KeyStore enumerator
        slice                   _key;                   // Current key
        slice                   _value;                 // Current value
        alloc_slice             _docID;                 // Current docID
        sequence_t              _sequence;              // Current sequence
    };

}

#endif /* defined(__CBForest__Index__) */
