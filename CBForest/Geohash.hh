
#ifndef __CBForest__Geohash__
#define __CBForest__Geohash__
#include "slice.hh"


namespace geohash {

    struct hash;

    /** A 2D geographic coordinate: (latitude, longitude). */
    struct coord {
        double latitude;
        double longitude;

        coord( )                            :latitude(0), longitude(0) { }
        coord(double lat, double lon)       :latitude(lat), longitude(lon) { }

        bool isValid() const;
        double distanceTo(coord) const;
        inline hash encode(unsigned nChars) const;
    };

    /** A range of a single coordinate. */
    struct range {
        double min;
        double max;

        range( )                            :min(0), max(0) { }
        range(double _min, double _max)     :min(_min), max(_max) { }

        bool isValid() const                {return max > min;}
        void normalize();

        bool contains(double n) const       {return min <= n && n < max;}
        inline bool intersects(range) const;

        double mid() const                  {return (min + max) / 2.0;}
        bool shrink(double);
        void shrink(bool side);
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
        inline bool intersects(area a) const;

        coord min() const                   {return coord(latitude.min, longitude.min);}
        coord mid() const                   {return coord(latitude.mid(), longitude.mid());}
        coord max() const                   {return coord(latitude.max, longitude.max);}
    };

    /** A GeoHash string. */
    struct hash {
        static const size_t kMaxLength = 22;

        char string[kMaxLength+1];

        hash()                              {(string)[0] = '\0';}
        hash(forestdb::slice);
        hash(const char *str)               {strlcpy(string, str, sizeof(string));}
        hash(coord, unsigned nChars);

        operator const char*() const        {return string;}
        size_t length() const               {return strlen(string);}
        size_t commonChars(const hash&);

        area decode() const;
        bool isValid() const;
    };

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

#if 0 // unused
    struct neighbors {
        char* north;
        char* east;
        char* west;
        char* south;
        char* north_east;
        char* south_east;
        char* north_west;
        char* south_west;
    };

    enum direction {
        NORTH = 0,
        EAST,
        WEST,
        SOUTH
    };

    neighbors* get_neighbors(const char *hash);
    void free_neighbors(neighbors *neighbors);
    char* get_adjacent(const char* hash, direction dir, char *adjacent);
#endif

}

#endif
