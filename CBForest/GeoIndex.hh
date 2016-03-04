//
//  GeoIndex.hh
//  CBForest
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__GeoIndex__
#define __CBForest__GeoIndex__
#include "MapReduceIndex.hh"
#include "Geohash.hh"
#include "KeyStore.hh"
#include <set>


namespace cbforest {

    CollatableBuilder& operator<< (CollatableBuilder&, const geohash::area&);
    geohash::area readGeoArea(CollatableReader&);


    class GeoIndexEnumerator : public IndexEnumerator {
    public:
        GeoIndexEnumerator(Index*, geohash::area);

        geohash::area keyBoundingBox() const    {return _keyBBox;}
        slice keyGeoJSON() const                {return _geoKey;}

#if DEBUG
        virtual ~GeoIndexEnumerator();
#endif

    protected:
        virtual bool approve(slice key); // override

    private:
        typedef std::pair<std::string, cbforest::sequence> ItemID;

        const geohash::area _searchArea;
        geohash::area _keyBBox;
        alloc_slice _geoKey;
        alloc_slice _geoValue;
        std::set<ItemID> _alreadySeen;

        unsigned _hits {0}, _misses {0}, _dups {0};   // Only used for test/profiling purposes
    };

}

#endif /* defined(__CBForest__GeoIndex__) */
