// NOTE: Heavily adapted from Lyo Kato's original geohash.c:
// https://github.com/lyokato/objc-geohash/blob/master/Classes/ARC/cgeohash.m as of 3-Nov-2014

/*
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
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <math.h>


namespace geohash {

static const char BASE32_ENCODE_TABLE[33] = "0123456789bcdefghjkmnpqrstuvwxyz";
static const char BASE32_DECODE_TABLE[44] = {
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


static inline double sqr(double d) { return d * d; }
static inline double deg2rad(double deg) { return deg / 180.0 * M_PI; }

double coord::distanceTo(coord c) const {
    // See http://en.wikipedia.org/wiki/Great-circle_distance
    static const double kEarthRadius = 6371.0;

    double lat1 = deg2rad(latitude), lat2 = deg2rad(c.latitude);
    double dLon = deg2rad(c.longitude - longitude);

    double angle = atan2( sqrt( sqr(cos(lat2) * sin(dLon))
                              + sqr(cos(lat1)*sin(lat2) - sin(lat1)*cos(lat2)*cos(dLon)) ),
                          sin(lat1)*sin(lat2) + cos(lat1)*cos(lat2)*cos(dLon) );
    return kEarthRadius * angle;
}


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

bool coord::isValid() const {
    return latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
}

area::area(coord c1, coord c2)
:latitude(c1.latitude, c2.latitude),
 longitude(c1.longitude, c2.longitude)
{ }



hash::hash(forestdb::slice bytes) {
    size_t n = std::min(bytes.size, sizeof(string) - 1);
    memcpy(string, bytes.buf, n);
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
        char bits = BASE32_DECODE_TABLE[c];
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
    assert(len <= hash::kMaxLength);
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

size_t hash::commonChars(const hash &h) {
    size_t n = std::min(length(), h.length());
    size_t i;
    for (i=0; i<n; i++)
        if (string[i] != h.string[i])
            break;
    return i;
}


#if 0 // unused
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
    
    
neighbors*
get_neighbors(const char* hash)
{
    neighbors *n;

    n = new neighbors;

    n->north = get_adjacent(hash, NORTH, NULL);
    n->east  = get_adjacent(hash, EAST, NULL);
    n->west  = get_adjacent(hash, WEST, NULL);
    n->south = get_adjacent(hash, SOUTH, NULL);

    n->north_east = get_adjacent(n->north, EAST, NULL);
    n->north_west = get_adjacent(n->north, WEST, NULL);
    n->south_east = get_adjacent(n->south, EAST, NULL);
    n->south_west = get_adjacent(n->south, WEST, NULL);

    return n;
}

char*
get_adjacent(const char* hash, direction dir, char *base)
{
    size_t len, idx;
    const char *border_table, *neighbor_table;
    char *refined_base, *ptr, last;

    assert(hash != NULL);

    len  = (int)strlen(hash);
    last = (unsigned char)tolower(hash[len - 1]);
    idx  = dir * 2 + (len % 2);

    border_table = BORDERS_TABLE[idx];

    if (base == NULL) {
        base = new char[len + 1];
    }

    if (base != hash)
        strncpy(base, hash, len - 1);
    base[len-1] = '\0';

    if (strchr(border_table, last) != NULL) {
        refined_base = get_adjacent(base, dir, base);
        if (refined_base == NULL) {
            return NULL;
        }
    }

    neighbor_table = NEIGHBORS_TABLE[idx];

    ptr = strchr(neighbor_table, last);
    if (ptr == NULL) {
        return NULL;
    }
    idx = (int)(ptr - neighbor_table);
    len = (int)strlen(base);
    base[len] = BASE32_ENCODE_TABLE[idx];
    base[len+1] = '\0';
    return base;
}

void 
free_neighbors(neighbors *neighbors)
{
    free(neighbors->north);
    free(neighbors->east);
    free(neighbors->west);
    free(neighbors->south);
    free(neighbors->north_east);
    free(neighbors->south_east);
    free(neighbors->north_west);
    free(neighbors->south_west);
    free(neighbors);
}
#endif

}
