//
//  Geohash.hh
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


#ifndef __CBForest__Geohash__
#define __CBForest__Geohash__
#include "slice.hh"
#include <string.h>
#include <vector>


namespace geohash {

    struct hash;
    struct hashRange;

    /** A 2D geographic coordinate: (latitude, longitude). */
    struct coord {
        double latitude;
        double longitude;

        coord( )                            :latitude(0), longitude(0) { }
        coord(double lat, double lon)       :latitude(lat), longitude(lon) { }

        bool isValid() const;
        double distanceTo(coord) const;     /**< Distance in km between two coords */

        /** Compute GeoHash of given length, containing this point */
        inline hash encode(unsigned nChars) const;

        /** Compute GeoHash whose center is within a given distance of this point. */
        hash encodeWithKmAccuracy(double kmAccuracy) const;
    };

    /** A range of a single coordinate. */
    struct range {
        double min;
        double max;

        range( )                            :min(0), max(0) { }
        range(double _min, double _max)     :min(_min), max(_max) { }

        bool isValid() const                {return max > min;}
        void normalize();                   /**< Swaps min/max if they're in wrong order */

        bool contains(double n) const       {return min <= n && n < max;}
        inline bool intersects(range) const;
        bool isEmpty() const                {return min == max;}

        double size() const                 {return max - min;}
        double mid() const                  {return (min + max) / 2.0;}

        // internal:
        bool shrink(double);
        void shrink(bool side);
        unsigned maxCharsToEnclose(bool isVertical) const;
    };

    /** A 2D rectangular area, defined by ranges of latitude and longitude. */
    struct area {
        range latitude;
        range longitude;

        area( )                             { }
        area(range lat, range lon)          :latitude(lat), longitude(lon) { }
        area(coord c1, coord c2);

        bool isValid() const                {return latitude.isValid() && longitude.isValid();}
        void normalize()                    {latitude.normalize(); longitude.normalize();}

        inline bool contains(coord) const;
        inline bool intersects(area) const;
        bool isPoint() const                {return latitude.isEmpty() && longitude.isEmpty();}

        coord min() const                   {return coord(latitude.min, longitude.min);}
        coord mid() const                   {return coord(latitude.mid(), longitude.mid());}
        coord max() const                   {return coord(latitude.max, longitude.max);}

        /** Returns a vector of hashes that completely cover this area. */
        std::vector<hash> coveringHashes() const;

        std::vector<hash> coveringHashesOfLength(unsigned nChars, unsigned maxCount) const;

        /** Returns a sorted vector of hashRanges that completely cover this area. Will attempt to
            be as accurate as possible (using longer hashes) without exceeding the maxCount.
            @param maxCount  The maximum number of results to return. */
        std::vector<hashRange> coveringHashRanges(unsigned maxCount) const;

        /** Returns a sorted vector of hashRanges that completely cover this area.
            @param nChars  The character count of each hash; longer hashes are more accurate but
                it may take a lot more to cover the area. */
        std::vector<hashRange> coveringHashRangesOfLength(unsigned nChars) const;

        std::string dump() const;

        unsigned maxCharsToEnclose() const;
    };


    enum direction {
        NORTH = 0,
        EAST,
        WEST,
        SOUTH
    };

    /** A GeoHash string. */
    struct hash {
        static const size_t kMaxLength = 22;

        char string[kMaxLength+1];

        hash()                              {(string)[0] = '\0';}
        explicit hash(cbforest::slice);
        explicit hash(const char *str);
        hash(coord, unsigned nChars);       /**< Geohash of the given coord */

        /** Returns the length of GeoHash string needed to get a specific accuracy
            measured in degrees. */
        static unsigned nCharsForDegreesAccuracy(double accuracy);

        operator const char*() const        {return string;}
        size_t length() const               {return strlen(string);}
        bool isEmpty() const                {return string[0] == '\0';}

        area decode() const;
        bool isValid() const;

        hash adjacent(direction) const;

        bool operator< (const hash &h) const     {return strcmp(string, h.string) < 0;}
    };

    /** A range of consecutive GeoHash strings. */
    struct hashRange : private hash {
        unsigned count;

        hashRange(const hash &h, unsigned c)    :hash(h), count(c) { }

        operator const char*() const            {return string;}
        bool operator< (const hashRange &h) const    {return strcmp(string, h.string) < 0;}

        hash operator[] (unsigned i) const;
        const hash& first() const               {return *(const hash*)this;}
        hash last() const                       {return (*this)[count-1];}

        /** Tries to add a GeoHash to the end; if successful returns true. */
        bool add(const hash &h);
        bool compact();
    };


    // Inline method bodies:

    inline bool range::intersects(geohash::range r) const {
        return max > r.min && r.max > min;
    }

    inline hash coord::encode(unsigned nChars) const {
        return hash(*this, nChars);
    }

    inline bool area::contains(coord c) const {
        return latitude.contains(c.latitude) && longitude.contains(c.longitude);
    }

    inline bool area::intersects(area a) const {
        return latitude.intersects(a.latitude) && longitude.intersects(a.longitude);
    }

}

#endif
