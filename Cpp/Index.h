//
//  Index.h
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__Index__
#define __CBForest__Index__

#include "Database.h"

namespace forestdb {
    
    class Index;
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
    class Index : public Database {
    public:
        Index(std::string path, Database::openFlags, const Database::config&);

        bool update(Transaction& transaction,
                    slice docID, sequence docSequence,
                    std::vector<Collatable> keys, std::vector<Collatable> values);

        IndexEnumerator enumerate(slice startKey, slice startKeyDocID,
                                  slice endKey,   slice endKeyDocID,
                                  const Database::enumerationOptions* options) {
            return IndexEnumerator(this, startKey, startKeyDocID,
                                   endKey, endKeyDocID,
                                   startKey.compare(endKey) <= 0,
                                   options);
        }

    private:
        bool removeOldRowsForDoc(Transaction& transaction, slice docID);
    };

}

#endif /* defined(__CBForest__Index__) */
