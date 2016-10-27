//
//  GeoIndex.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "GeoIndex.hh"
#include "Logging.hh"
#include "Fleece.hh"
#include <math.h>
#include <set>
#include <algorithm>

// for nonstandard strlcat():
#include <string.h>
#if defined(linux) || defined(__linux__)
#include <bsd/string.h>
#elif defined(_MSC_VER)
#include "strlcat.h"
#endif

namespace litecore {

    extern LogDomain IndexLog;

    static const unsigned kMaxKeyRanges = 50;


    geohash::area readGeoArea(CollatableReader& reader) {
        geohash::area a;
        a.longitude.min = reader.readDouble();
        a.latitude.min = reader.readDouble();
        a.longitude.max = reader.readDouble();
        a.latitude.max = reader.readDouble();
        return a;
    }

    geohash::area readGeoArea(fleece::Array::iterator& iter) {
        geohash::area a;
        a.longitude.min = iter->asDouble();
        a.latitude.min  = (++iter)->asDouble();
        a.longitude.max = (++iter)->asDouble();
        a.latitude.max  = (++iter)->asDouble();
        ++iter;
        return a;
    }

    /** Given a geo area, returns a list of key (geohash) ranges that cover that area. */
    static std::vector<KeyRange> keyRangesFor(geohash::area a) {
        auto hashes = a.coveringHashRanges(kMaxKeyRanges);
        std::vector<KeyRange> ranges;
        for (auto &h : hashes) {
            geohash::hash lastHash = h.last();
            LogTo(IndexLog, "GeoIndexEnumerator: query add '%s' ... '%s'",
                (const char*)h.first(), (const char*)lastHash);
            strlcat(lastHash.string, "Z", sizeof(lastHash.string)); // so the string range includes everything inside lastHash
            ranges.push_back(KeyRange(CollatableBuilder(h.first()), CollatableBuilder(lastHash)));

            // Also need to look for all _exact_ parent hashes. For example, if the hashRange
            // is 9b1...9b7, we also want the exact keys "9b" and "9".
            geohash::hash parent = h.first();
            size_t len = strlen(parent.string);
            while (len > 1) {
                parent.string[--len] = '\0';
                CollatableBuilder key(parent);
                KeyRange range(key, key);
                if (std::find(ranges.begin(), ranges.end(), range) == ranges.end()) {
                    ranges.push_back(range);
                    LogTo(IndexLog, "GeoIndexEnumerator: query add '%s'", parent.string);
                }
            }
        }
        return ranges;
    }


    GeoIndexEnumerator::GeoIndexEnumerator(MapReduceIndex &index,
                                           geohash::area searchArea)
    :IndexEnumerator(index,
                     keyRangesFor(searchArea)),
     _searchArea(searchArea)
    { }

    bool GeoIndexEnumerator::approve(slice key) {
        // Have we seen this result before?
        _geoID = (unsigned)fleece::Value::fromTrustedData(value())->asUnsigned();
        ItemID item((std::string)recordID(), _geoID);
        if (_alreadySeen.find(item) != _alreadySeen.end()) {
            _dups++;
            return false;
        }
        _alreadySeen.insert(item);

        // Read the actual rect and see if it truly intersects the query:
        ((MapReduceIndex&)index()).readGeoArea(item.first, sequence(), _geoID,
                                                _keyBBox, _geoKey, _geoValue);
        if (!_keyBBox.intersects(_searchArea)) {
            _misses++;
            return false;
        }

        // OK, it's for reals.
        setValue(_geoValue);
        _hits++;
        return true;
    }

#if DEBUG
    GeoIndexEnumerator::~GeoIndexEnumerator() {
        LogTo(IndexLog, "GeoIndexEnumerator: %u hits, %u misses, %u dups, %u total iterated (of %llu keys)",
            _hits, _misses, _dups, _hits+_misses+_dups, ((MapReduceIndex&)index()).rowCount());
    }
#endif

}
