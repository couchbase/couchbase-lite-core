//
//  Index.h
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__Index__
#define __CBForest__Index__

#include "Database.hh"

namespace forestdb {
    
    class Index;
    class IndexTransaction;
    class Collatable;


    /** Index query enumerator. */
    class IndexEnumerator {
    public:
        slice key() const                       {return _key;}
        slice value() const                     {return _value;}
        slice docID() const                     {return _docID;}
        sequence sequence() const               {return _sequence;}

        bool next();
        const IndexEnumerator& operator++()     {next(); return *this;}
        operator bool() const                   {return _dbEnum;}

    private:
        friend class Index;
        IndexEnumerator(Index*, slice startKey, slice startKeyDocID,
                        slice endKey, slice endKeyDocID,
                        bool ascending,
                        const Database::enumerationOptions*);
        void read();

        Index* _index;
        alloc_slice _endKey;
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

        bool update(IndexTransaction& transaction,
                    slice docID, sequence docSequence,
                    std::vector<Collatable> keys, std::vector<Collatable> values);

        IndexEnumerator enumerate(Collatable startKey, slice startKeyDocID,
                                  Collatable endKey,   slice endKeyDocID,
                                  const Database::enumerationOptions* options);

        IndexEnumerator enumerate(std::vector<std::string> keys,
                                  const Database::enumerationOptions* options);

    private:
        bool removeOldRowsForDoc(Transaction& transaction, slice docID);

        friend class IndexTransaction;
        friend class IndexEnumerator;
    };

    class IndexTransaction : protected Transaction {
    public:
        IndexTransaction(Index* index)              :Transaction(index) {}

        void erase()                                {Transaction::erase();}

        friend class Index;
        friend class MapReduceIndex;
    };

}

#endif /* defined(__CBForest__Index__) */
