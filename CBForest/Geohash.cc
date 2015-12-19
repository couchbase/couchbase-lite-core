//
//  Geohash.cc
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

/* NOTE: Portions of this code derive from Lyo Kato's geohash.c:
   https://github.com/lyokato/objc-geohash/blob/master/Classes/ARC/cgeohash.m as of 3-Nov-2014
   That code comes with the following license:

The MIT License

Copyright (c) 2011 lyo.kato@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "Geohash.hh"
#include "Error.hh"
#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <sstream>


namespace geohash {

static const char BASE32_ENCODE_TABLE[33] = "0123456789bcdefghjkmnpqrstuvwxyz";
static const int8_t BASE32_DECODE_TABLE[44] = {
    /* 0 */   0, /* 1 */   1, /* 2 */   2, /* 3 */   3, /* 4 */   4,    
    /* 5 */   5, /* 6 */   6, /* 7 */   7, /* 8 */   8, /* 9 */   9,    
    /* : */  -1, /* ; */  -1, /* < */  -1, /* = */  -1, /* > */  -1, 
    /* ? */  -1, /* @ */  -1, /* A */  -1, /* B */  10, /* C */  11, 
    /* D */  12, /* E */  13, /* F */  14, /* G */  15, /* H */  16, 
    /* I */  -1, /* J */  17, /* K */  18, /* L */  -1, /* M */  19, 
    /* N */  20, /* O */  -1, /* P */  21, /* Q */  22, /* R */  23, 
    /* S */  24, /* T */  25, /* U */  26, /* V */  27, /* W */  28, 
    /* X */  29, /* Y */  30, /* Z */  31    
};


static const double CELL_WIDTHS[22] = {
    45.0,
    11.25,
    1.40625,
    0.3515625,
    0.0439453125,
    0.010986328125,
    0.001373291015625,
    0.00034332275390625,
    4.291534423828125e-05,
    1.0728836059570312e-05,
    1.341104507446289e-06,
    3.3527612686157227e-07,
    4.190951585769653e-08,
    1.0477378964424133e-08,
    1.3096723705530167e-09,
    3.2741809263825417e-10,
    4.092726157978177e-11,
    1.0231815394945443e-11, 
    1.2789769243681803e-12, 
    3.197442310920451e-13, 
    3.9968028886505635e-14, 
    9.992007221626409e-15,
};

static const double CELL_HEIGHTS[22] = {
    45.0,
    5.625,
    1.40625,
    0.17578125,
    0.0439453125,
    0.0054931640625,
    0.001373291015625,
    0.000171661376953125,
    4.291534423828125e-05,
    5.364418029785156e-06,
    1.341104507446289e-06,
    1.6763806343078613e-07,
    4.190951585769653e-08,
    5.238689482212067e-09,
    1.3096723705530167e-09,
    1.6370904631912708e-10,
    4.092726157978177e-11,
    5.115907697472721e-12, 
    1.2789769243681803e-12, 
    1.5987211554602254e-13, 
    3.9968028886505635e-14, 
    4.9960036108132044e-15,
};

// Approximation (Earth isn't actually a sphere), in km
static const double kEarthRadius = 6371.0;

// Kilometers per degree of latitude, and per degree of longitude at the equator
static const double kKmPerDegree = 2 * M_PI * kEarthRadius / 360.0;


static inline double sqr(double d)          { return d * d; }
static inline double deg2rad(double deg)    { return deg / 180.0 * M_PI; }


#pragma mark - COORD


bool coord::isValid() const {
    return latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
}

double coord::distanceTo(coord c) const {
    // See http://en.wikipedia.org/wiki/Great-circle_distance

    double lat1 = deg2rad(latitude), lat2 = deg2rad(c.latitude);
    double dLon = deg2rad(c.longitude - longitude);

    double angle = atan2( sqrt( sqr(cos(lat2)*sin(dLon))
                              + sqr(cos(lat1)*sin(lat2) - sin(lat1)*cos(lat2)*cos(dLon)) ),
                          sin(lat1)*sin(lat2) + cos(lat1)*cos(lat2)*cos(dLon) );
    return kEarthRadius * angle;
}

hash coord::encodeWithKmAccuracy(double accuracyInKm) const {
    // Rough approximation: start with nChars that gives small enough cell height
    double minDegreeHeight = 2 * accuracyInKm / kKmPerDegree;
    unsigned nChars;
    for (nChars = 1; nChars <= hash::kMaxLength; nChars++) {
        if (CELL_HEIGHTS[nChars-1] <= minDegreeHeight)
            break;
    }

    // Now encode with more and more characters until the encoded area's center is close enough:
    hash h;
    for (; nChars <= hash::kMaxLength; nChars++) {
        h = encode(nChars);
        coord decoded = h.decode().mid();
        double error = distanceTo(decoded);
        if (error <= accuracyInKm)
            break;
    }
    return h;
}


#pragma mark - RANGE:


void range::normalize() {
    if (max < min)
        std::swap(max, min);
}

void range::shrink(bool side) {
    double m = mid();
    if (side) {
        min = m;
    } else {
        max = m;
    }
}

bool range::shrink(double value) {
    bool side = value >= mid();
    shrink(side);
    return side;
}


// Returns the length of the longest geohash prefix that could completely contain this range.
unsigned range::maxCharsToEnclose(bool isVertical) const {
    const double size = max - min;
    const double *cellsize = (isVertical ? CELL_HEIGHTS : CELL_WIDTHS);
    unsigned chars;
    for (chars=0; chars < 16; ++chars)
        if (size > cellsize[chars])
            break;
    return chars;
}


#pragma mark - AREA:


area::area(coord c1, coord c2)
:latitude(c1.latitude, c2.latitude),
 longitude(c1.longitude, c2.longitude)
{ }

unsigned area::maxCharsToEnclose() const {
    return std::min(latitude.maxCharsToEnclose(false), longitude.maxCharsToEnclose(true));
}

std::vector<hash> area::coveringHashes() const {
    static unsigned kMaxCount = 9;  // Heuristically chosen
    unsigned nChars = maxCharsToEnclose();
    std::vector<hash> result;
    result = coveringHashesOfLength(nChars + 1, kMaxCount);
    if (result.size() == 0 && nChars > 0)
        result = coveringHashesOfLength(nChars, kMaxCount);
    return result;
}

std::vector<hash> area::coveringHashesOfLength(unsigned nChars, unsigned maxCount) const {
    std::vector<hash> covering;
    hash sw = coord(latitude.min, longitude.min).encode(nChars);
    area swArea = sw.decode();
    unsigned nRows = (unsigned)ceil((latitude.max  - swArea.latitude.min) /swArea.latitude.size());
    unsigned nCols = (unsigned)ceil((longitude.max - swArea.longitude.min)/swArea.longitude.size());
    if (nRows * nCols <= maxCount) {
        // Generate all the geohashes in a raster scan:
        for (unsigned row = 0; row < nRows; ++row) {
            if (row > 0) {
                sw = sw.adjacent(NORTH);
                if (sw.isEmpty())
                    break;
            }
            hash h = sw;
            for (unsigned col = 0; col < nCols; ++col) {
                if (col > 0) {
                    h = h.adjacent(EAST);
                    if (h.isEmpty())
                        break;
                }
                covering.push_back(h);
            }
        }
    }
    return covering;
}

std::vector<hashRange> area::coveringHashRanges(unsigned maxCount) const {
    unsigned nChars = std::max(maxCharsToEnclose(), 1u);
    std::vector<hashRange> result;
    for (; nChars <= hash::kMaxLength; nChars++) {
        std::vector<hashRange> covering = coveringHashRangesOfLength(nChars);
        if (covering.size() > maxCount)
            break;
        result = covering;
    }
    return result;
}

std::vector<hashRange> area::coveringHashRangesOfLength(unsigned nChars) const {
    std::vector<hash> covering = coveringHashesOfLength(nChars, UINT32_MAX);

    // Sort hashes by string value:
    std::sort(covering.begin(), covering.end());

    // Coalesce the hashes into hashRanges:
    std::vector<hashRange> result;
    for (auto h = covering.begin(); h != covering.end(); ++h) {
        if (result.size() > 0 && result.back().add(*h)) {
            // Check if result.back() can itself be coalesced
            if (result.back().compact()) {
                while (result.size() > 1 && result[result.size()-2].add(result.back().first())) {
                    result.pop_back();
                    result.back().compact();
                }
            }
        } else {
            result.push_back(hashRange(*h, 1));
        }
    }
    return result;
}


#pragma mark - HASH, HASHRANGE:


hash::hash(cbforest::slice bytes) {
    size_t n = std::min(bytes.size, sizeof(string) - 1);
    memcpy(string, bytes.buf, n);
    string[n] = '\0';
}

hash::hash(const char *str) {
    size_t n = std::min(strlen(str), sizeof(string) - 1);
    memcpy(string, str, n);
    string[n] = '\0';
}


bool hash::isValid() const {
    const char *p;
    unsigned char c;
    p = &string[0];
    if (!*p)
        return false; // empty
    while (*p) {
        c = (unsigned char)toupper(*p++) - 0x030;
        if (c > 43)
            return false;
        if (BASE32_DECODE_TABLE[c] == -1)
            return false;
    }
    return true;
}

static inline void refineRange(range &r, unsigned char bits, int offset) {
    r.shrink( (bits & (0x1 << offset)) != 0);
}
    
area hash::decode() const {
    area result(range(-90, 90), range(-180, 180));
    range *range1 = &result.longitude;
    range *range2 = &result.latitude;

    for (const char *p = &string[0]; *p; ++p) {
        unsigned char c = (unsigned char)toupper(*p) - 0x030;
        if (c > 43) {
            return area();  // invalid hash
        }
        int8_t bits = BASE32_DECODE_TABLE[c];
        if (bits == -1) {
            return area();  // invalid hash
        }

        refineRange(*range1, bits, 4);
        refineRange(*range2, bits, 3);
        refineRange(*range1, bits, 2);
        refineRange(*range2, bits, 1);
        refineRange(*range1, bits, 0);

        std::swap(range1, range2);
    }
    return result;
}

static inline void setBit(unsigned char &bits, range &r, double value, int offset) {
    if (r.shrink(value))
        bits |= (0x1 << offset);
}
    
hash::hash(coord c, unsigned len)
{
    CBFAssert(len <= hash::kMaxLength);
    if (!c.isValid()) {
        string[0] = '\0';
        return; // invalid coord, so return invalid hash
    }

    range lat_range(-90, 90);
    range lon_range(-180, 180);
    range *range1 = &lon_range;
    range *range2 = &lat_range;
    double val1 = c.longitude;
    double val2 = c.latitude;

    for (int i=0; i < len; i++) {
        unsigned char bits = 0;
        setBit(bits, *range1, val1, 4);
        setBit(bits, *range2, val2, 3);
        setBit(bits, *range1, val1, 2);
        setBit(bits, *range2, val2, 1);
        setBit(bits, *range1, val1, 0);
        string[i] = BASE32_ENCODE_TABLE[bits];

        std::swap(val1, val2);
        std::swap(range1, range2);
    }

    string[len] = '\0';
}

/*static*/ unsigned hash::nCharsForDegreesAccuracy(double accuracy) {
    unsigned nChars;
    for (nChars = 1; nChars < hash::kMaxLength; nChars++) {
        if (CELL_HEIGHTS[nChars-1] <= accuracy && CELL_WIDTHS[nChars-1] <= accuracy)
            break;
    }
    return nChars;
}
    

// Adds n to a geohash character, returning the resulting character or \0 if out of range
static char addChar(char c, unsigned n) {
    unsigned char uc = (unsigned char)toupper(c) - 0x030;
    CBFAssert(uc < 44);
    int index = BASE32_DECODE_TABLE[uc];
    CBFAssert(index >= 0);
    index += n;
    if (index >= 32)
        return 0;
    return BASE32_ENCODE_TABLE[index];
}


bool hashRange::add(const hash &h) {
    size_t len = strlen(string);
    if (memcmp(h.string, string, len-1) == 0
            && h.string[len] == '\0'
            && h.string[len-1] == addChar(string[len-1], count)) {
        ++count;
        return true;
    }
    return false;
}

bool hashRange::compact() {
    if (count == 32) {
        size_t len = strlen(string);
        if (len > 0) {
            string[len-1] = '\0';
            count = 1;
            return true;
        }
    }
    return false;
}

hash hashRange::operator[](unsigned i) const {
    CBFAssert(i < count);
    hash h = *(const hash*)this;
    if (i > 0) {
        size_t max = strlen(h.string)-1;
        h.string[max] = addChar(h.string[max], i);
    }
    return h;
}


#pragma mark - UTILITIES:


static const char NEIGHBORS_TABLE[8][33] = {
    "p0r21436x8zb9dcf5h7kjnmqesgutwvy", /* NORTH EVEN */
    "bc01fg45238967deuvhjyznpkmstqrwx", /* NORTH ODD  */
    "bc01fg45238967deuvhjyznpkmstqrwx", /* EAST EVEN  */
    "p0r21436x8zb9dcf5h7kjnmqesgutwvy", /* EAST ODD   */
    "238967debc01fg45kmstqrwxuvhjyznp", /* WEST EVEN  */
    "14365h7k9dcfesgujnmqp0r2twvyx8zb", /* WEST ODD   */
    "14365h7k9dcfesgujnmqp0r2twvyx8zb", /* SOUTH EVEN */
    "238967debc01fg45kmstqrwxuvhjyznp"  /* SOUTH ODD  */
};

static const char BORDERS_TABLE[8][9] = {
    "prxz",     /* NORTH EVEN */
    "bcfguvyz", /* NORTH ODD */
    "bcfguvyz", /* EAST  EVEN */
    "prxz",     /* EAST  ODD */
    "0145hjnp", /* WEST  EVEN */
    "028b",     /* WEST  ODD */
    "028b",     /* SOUTH EVEN */
    "0145hjnp"  /* SOUTH ODD */
};

static bool
get_adjacent(const char* hash, direction dir, char *base)
{
    size_t len, idx;
    const char *border_table, *neighbor_table, *ptr;
    char last;

    len  = (int)strlen(hash);
    if (len == 0)
        return false;
    last = (unsigned char)tolower(hash[len - 1]);
    idx  = dir * 2 + (len % 2);

    border_table = BORDERS_TABLE[idx];

    if (base != hash)
        strncpy(base, hash, len - 1);
    base[len-1] = '\0';

    if (strchr(border_table, last) != NULL) {
        if (!get_adjacent(base, dir, base))
            return false;
    }

    neighbor_table = NEIGHBORS_TABLE[idx];

    ptr = strchr(neighbor_table, last);
    if (ptr == NULL) {
        return false;
    }
    idx = (int)(ptr - neighbor_table);
    len = (int)strlen(base);
    base[len] = BASE32_ENCODE_TABLE[idx];
    base[len+1] = '\0';
    return true;
}

hash hash::adjacent(direction dir) const {
    hash result;
    get_adjacent(string, dir, result.string);
    return result;
}


std::string area::dump() const {
    std::stringstream out;
    out << "(" << latitude.min << ", " << longitude.min << ")...("
                << latitude.max << ", " << longitude.max << ")";
    return out.str();
}



}
