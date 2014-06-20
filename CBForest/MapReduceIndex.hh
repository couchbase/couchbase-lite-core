//
//  MapReduceIndex.hh
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__MapReduceIndex__
#define __CBForest__MapReduceIndex__

#include "Index.hh"

namespace forestdb {

    class EmitFn {
    public:
        virtual void operator() (Collatable key, Collatable value) =0;
    };

    class MapFn {
    public:
        virtual void operator() (const Document&, EmitFn& emit) =0;
    };

    /** An Index that uses a MapFn to index the documents of another Database. */
    class MapReduceIndex : public Index {
    public:
        MapReduceIndex(std::string path,
                       Database::openFlags,
                       const Database::config&,
                       Database* sourceDatabase);

        void readState();
        int indexType() const                   {return _indexType;}
        
        void setup(int indexType, MapFn *map, std::string mapVersion);

        sequence lastSequenceIndexed() const    {return _lastSequenceIndexed;}
        sequence lastSequenceChangedAt() const  {return _lastSequenceChangedAt;}

        void updateIndex();

    private:
        void invalidate();
        void saveState(IndexTransaction& t);
        
        forestdb::Database* _sourceDatabase;
        MapFn* _map;
        std::string _mapVersion, _lastMapVersion;
        int _indexType;
        sequence _lastSequenceIndexed, _lastSequenceChangedAt;
    };
}

#endif /* defined(__CBForest__MapReduceIndex__) */
