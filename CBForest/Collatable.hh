//
//  Collatable.hh
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__Collatable__
#define __CBForest__Collatable__
#include <stdint.h>
#include <string>
#include <iostream>
#include "slice.hh"
#include "Geohash.hh"

namespace cbforest {

    class CollatableTypes {
    public:
        typedef enum {
            kEndSequence = 0,   // Returned to indicate the end of an array/dict
            kNull,
            kFalse,
            kTrue,
            kNegative,
            kPositive,
            kString,
            kArray,
            kMap,
            kGeohash,           // Geohash string
            kSpecial,           // Placeholder for doc (Only used in values, not keys)
            kFullTextKey,       // String to be full-text-indexed (Only used in emit() calls)
            kGeoJSONKey,        // GeoJSON to be indexed (only used in emit() calls)
            kError = 255        // Something went wrong. (Never stored, only returned from peekTag)
        } Tag;
    };

    class CollatableBuilder;

    class Collatable : public alloc_slice, public CollatableTypes {
    public:
        Collatable()                :alloc_slice()  { }
        explicit inline Collatable(CollatableBuilder&& b);

        static Collatable withData(slice s)         {return Collatable(s, true);}
        static Collatable withData(alloc_slice s)   {return Collatable(s, true);}

        slice data() const          {return (slice)*this;}

        bool empty() const          {return size == 0;}

        std::string toJSON() const;

    private:
        Collatable(alloc_slice s, bool)     :alloc_slice(s) { }
        Collatable(slice s, bool)           :alloc_slice(s) { }
};

    
    /** A binary encoding of JSON-compatible data, that collates with CouchDB-compatible semantics
        using a dumb binary compare (like memcmp).
        Data format spec: https://github.com/couchbaselabs/cbforest/wiki/Collatable-Data-Format
        Collatable owns its data, in the form of a C++ string object. */
    class CollatableBuilder : public CollatableTypes {
    public:
        CollatableBuilder();
        explicit CollatableBuilder(Collatable c); // Imports data previously saved in collatable format
        CollatableBuilder(slice, bool);        // Imports data previously saved in collatable format
        ~CollatableBuilder();

        template<typename T> explicit CollatableBuilder(const T &t)
        :_buf(slice::newBytes(kDefaultSize), kDefaultSize),
         _available(_buf)
        {
            *this << t;
        }

        CollatableBuilder& addNull()                       {addTag(kNull); return *this;}
        CollatableBuilder& addBool (bool); // overriding <<(bool) is dangerous due to implicit conversion

        CollatableBuilder& operator<< (double);

        CollatableBuilder& operator<< (const Collatable&);
        CollatableBuilder& operator<< (const CollatableBuilder&);
        CollatableBuilder& operator<< (std::string s)      {return operator<<(slice(s));}
        CollatableBuilder& operator<< (const char* cstr)   {return operator<<(slice(cstr));}
        CollatableBuilder& operator<< (slice s)            {addString(kString, s); return *this;}

        CollatableBuilder& operator<< (const geohash::hash& h) {addString(kGeohash, (slice)h);
                                                                return *this;}

        CollatableBuilder& addFullTextKey(slice text, slice languageCode = slice::null);
        CollatableBuilder& addGeoKey(slice geoJSON, geohash::area bbox);

        CollatableBuilder& beginArray()                    {addTag(kArray); return *this;}
        CollatableBuilder& endArray()                      {addTag(kEndSequence); return *this;}

        CollatableBuilder& beginMap()                      {addTag(kMap); return *this;}
        CollatableBuilder& endMap()                        {addTag(kEndSequence); return *this;}

#ifdef __OBJC__
        CollatableBuilder(id obj);
        CollatableBuilder& operator<< (id);
#endif

        CollatableBuilder& addSpecial()                    {addTag(kSpecial); return *this;}

        size_t size() const                         {return _buf.size - _available.size;}
        bool empty() const                          {return size() == 0;}

        std::string toJSON() const;

        slice data() const                          {return slice(_buf.buf, size());}
        operator slice() const                      {return data();}

        operator Collatable () const                {return Collatable::withData(data());}

        alloc_slice extractOutput() {
            auto result = data();
            _buf = _available = slice::null;
            return alloc_slice::adopt(result);
        }

        CollatableBuilder(CollatableBuilder&& c) {
            _buf = c._buf;
            _available = c._available;
            c._buf.buf = NULL;
        }
        CollatableBuilder& operator= (CollatableBuilder &&c) {
            _buf = c._buf;
            _available = c._available;
            c._buf.buf = NULL;
            return *this;
        }

    private:
        static const size_t kMinSize = 32;
        static const size_t kDefaultSize = 128;

        CollatableBuilder(const CollatableBuilder& c);
        CollatableBuilder& operator= (const CollatableBuilder &c);

        uint8_t* reserve(size_t amt);
        void add(slice);
        void addTag(Tag t)                          {uint8_t c = t; add(slice(&c,1));}
        void addString(Tag, slice);

        slice _buf;
        slice _available;
    };


    /** A decoder of Collatable-format data. Does _not_ own its data (reads from a slice.) */
    class CollatableReader : public CollatableTypes {
    public:
        CollatableReader(slice s);

        slice data() const                  {return _data;}
        bool atEnd() const                  {return _data.size == 0;}
        
        Tag peekTag() const;
        void skipTag()                      {if (_data.size > 0) _skipTag();}

        int64_t readInt();
        double readDouble();
        alloc_slice readString()            {return readString(kString);}
        geohash::hash readGeohash();
        
        std::pair<alloc_slice, alloc_slice> readFullTextKey();  // pair is <text, langCode>
        alloc_slice readGeoKey(geohash::area &outBBox);

#ifdef __OBJC__
        id readNSObject();
        NSString* readNSString();
#endif

        /** Reads (skips) an entire object of any type, returning its data in Collatable form. */
        slice read();

        void beginArray();
        void endArray();
        void beginMap();
        void endMap();

        void writeJSONTo(std::ostream &out);
        std::string toJSON();

        static uint8_t* getInverseCharPriorityMap();

    private:
        void expectTag(Tag tag);
        void _skipTag()                     {_data.moveStart(1);} // like skipTag but unsafe
        alloc_slice readString(Tag);

        slice _data;
    };


    Collatable::Collatable(CollatableBuilder&& b) :alloc_slice(b.extractOutput()) { }


}

#endif /* defined(__CBForest__Collatable__) */
