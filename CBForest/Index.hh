//
//  Index.hh
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

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

        bool update(slice docID,
                    sequence docSequence,
                    std::vector<Collatable> keys,
                    std::vector<Collatable> values);

        void erase()                                {Transaction::erase();}

    private:
        bool removeOldRowsForDoc(slice docID);

        friend class Index;
        friend class MapReduceIndex;
    };

}

#endif /* defined(__CBForest__Index__) */
