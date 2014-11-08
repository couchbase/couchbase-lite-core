//
//  GeoIndex.cpp
//  CBForest
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "GeoIndex.hh"
#include "LogInternal.hh"
#include <math.h>


namespace forestdb {

    static const int kDefaultHashLength = 8;
    static const unsigned kMaxKeyRanges = 50;

    Collatable& operator<< (Collatable &coll, geohash::coord c) {
        coll << geohash::hash(c, kDefaultHashLength);
        return coll;
    }

    
    static std::vector<KeyRange> keyRangesFor(geohash::area a) {
        auto hashes = a.coveringHashes(kMaxKeyRanges);
        std::vector<KeyRange> ranges;
        for (auto h = hashes.begin(); h != hashes.end(); ++h) {
            auto lastHash = h->lastHash();
            strcat(lastHash.string, "Z");
            ranges.push_back(KeyRange(Collatable(h->string), Collatable(lastHash.string)));
        }
        return ranges;
    }


    GeoIndexEnumerator::GeoIndexEnumerator(Index &index,
                                           geohash::area searchArea)
    :IndexEnumerator(index,
                     keyRangesFor(searchArea),
                     DocEnumerator::Options::kDefault,
                     false),
     _searchArea(searchArea)
    {
#if DEBUG
        _count[false] = _count[true] = 0;
#endif
        read();
    }


    bool GeoIndexEnumerator::approve(slice key) {
        // Check that the row's area actually intersects the search area:
        CollatableReader reader(key);
        geohash::hash hash(reader.readString());
        _keyArea = hash.decode();
        bool result = _searchArea.intersects(_keyArea);
#if DEBUG
        _count[result]++;
#endif
        return result;
    }

#if DEBUG
    GeoIndexEnumerator::~GeoIndexEnumerator() {
        Warn("enum: %u hits, %u misses (%g%%)", _count[1], _count[0],
              floor(_count[1]/(double)(_count[1]+_count[0])*100.0));
    }
#endif

}