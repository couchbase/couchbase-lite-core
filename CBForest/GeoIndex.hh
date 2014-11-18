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


namespace forestdb {

    Collatable& operator<< (Collatable&, geohash::coord);


    class GeoIndexEnumerator : public IndexEnumerator {
    public:
        GeoIndexEnumerator(Index*, geohash::area);
        GeoIndexEnumerator(Index*,
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
