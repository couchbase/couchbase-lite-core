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

#include "DocEnumerator.hh"
#include "Collatable.hh"

namespace forestdb {
    
    class Index;
    class IndexTransaction;
    class Collatable;


    /** Index query enumerator. */
    class IndexEnumerator {
    public:
        IndexEnumerator(Index&,
                        Collatable startKey, slice startKeyDocID,
                        Collatable endKey, slice endKeyDocID,
                        const DocEnumerator::Options&);

        IndexEnumerator(Index&,
                        std::vector<Collatable> keys,
                        const DocEnumerator::Options&);

        CollatableReader key() const            {return CollatableReader(_key);}
        CollatableReader value() const          {return CollatableReader(_value);}
        slice docID() const                     {return _docID;}
        sequence sequence() const               {return _sequence;}

        bool next();
        const IndexEnumerator& operator++()     {next(); return *this;}
        operator bool() const                   {return _dbEnum;}

    private:
        friend class Index;
        bool read();
        bool nextKey();

        Index& _index;
        DocEnumerator::Options _options;
        alloc_slice _endKey;
        bool _inclusiveEnd;
        std::vector<Collatable> _keys;
        int _currentKeyIndex;

        DocEnumerator _dbEnum;
        slice _key;
        slice _value;
        alloc_slice _docID;
        ::forestdb::sequence _sequence;
    };


    /** A database used as an index. */
    class Index : protected Database {
    public:
        Index(std::string path, Database::openFlags, const config&);

        typedef Database::config config;
        static config defaultConfig()           {return Database::defaultConfig();}

    private:
        friend class IndexTransaction;
        friend class IndexEnumerator;
    };


    /** A transaction to update an index. */
    class IndexTransaction : protected Transaction {
    public:
        IndexTransaction(Index* index)              :Transaction(index) {}

        Index* index() const                        {return (Index*)database();}

        /** Updates the index entry for a document with the given keys and values.
            Adjusts the value of rowCount by the number of rows added or removed.
            Returns true if the index may have changed as a result. */
        bool update(slice docID,
                    sequence docSequence,
                    std::vector<Collatable> keys,
                    std::vector<Collatable> values,
                    uint64_t &rowCount);

        void erase()                                {Transaction::erase();}

    private:
        int64_t removeOldRowsForDoc(slice docID);

        friend class Index;
        friend class MapReduceIndex;
    };

}

#endif /* defined(__CBForest__Index__) */
