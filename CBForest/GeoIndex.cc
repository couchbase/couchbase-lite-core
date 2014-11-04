//
//  GeoIndex.cpp
//  CBForest
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "GeoIndex.hh"

namespace forestdb {

    static const int kDefaultHashLength = 8;

    Collatable& operator<< (Collatable &coll, geohash::coord c) {
        coll << geohash::hash(c, kDefaultHashLength);
        return coll;
    }

    
    static inline Collatable keyFor(geohash::coord c) {
        Collatable coll;
        return coll << c;
    }


    GeoIndexEnumerator::GeoIndexEnumerator(Index &index,
                                           geohash::area searchArea)
    :IndexEnumerator(index,
                     keyFor(searchArea.min()), slice::null,
                     keyFor(searchArea.max()), slice::null,
                     DocEnumerator::Options::kDefault),
     _searchArea(searchArea)
    { }


    GeoIndexEnumerator::GeoIndexEnumerator(Index &index,
                                           geohash::area searchArea,
                                           slice startKeyDocID, slice endKeyDocID,
                                           const DocEnumerator::Options& options)
    :IndexEnumerator(index,
                     keyFor(searchArea.min()), startKeyDocID,
                     keyFor(searchArea.max()), endKeyDocID,
                     options),
     _searchArea(searchArea)
    { }


    bool GeoIndexEnumerator::approve(slice key) {
        // Check that the row's area actually intersects the search area:
        CollatableReader reader(key);
        geohash::hash hash(reader.readString());
        _keyArea = hash.decode();
        return _searchArea.intersects(_keyArea);
    }

}