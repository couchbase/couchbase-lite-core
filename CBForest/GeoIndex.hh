//
//  GeoIndex.h
//  CBForest
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__GeoIndex__
#define __CBForest__GeoIndex__
#include "MapReduceIndex.hh"
#include "Geohash.hh"


namespace forestdb {

    Collatable& operator<< (Collatable&, geohash::coord);


    class GeoIndexEnumerator : public IndexEnumerator {
    public:
        GeoIndexEnumerator(Index&, geohash::area);
        GeoIndexEnumerator(Index&,
                           geohash::area,
                           slice startKeyDocID, slice endKeyDocID,
                           const DocEnumerator::Options&);

        geohash::area keyArea() const           {return _keyArea;}
        geohash::coord keyCoord() const         {return _keyArea.mid();}

#if DEBUG
        ~GeoIndexEnumerator();
#endif

    protected:
        virtual bool approve(slice key); // override

    private:
        const geohash::area _searchArea;
        geohash::area _keyArea;
#if DEBUG
        unsigned _count[2];
#endif
    };

}

#endif /* defined(__CBForest__GeoIndex__) */
