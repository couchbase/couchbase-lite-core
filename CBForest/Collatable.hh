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

namespace forestdb {

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
            kSpecial,
            kError = 255        // Something went wrong...
        } Tag;
    };

    /** A binary encoding of JSON-compatible data, that collates with CouchDB-compatible semantics
        using a dumb binary compare (like memcmp).
        Collatable owns its data, in the form of a C++ string object. */
    class Collatable : public CollatableTypes {
    public:
        Collatable();
        Collatable(slice, bool);        // Imports data previously saved in collatable format

        template<typename T> explicit Collatable(const T &t)    {*this << t;}

        Collatable& addNull()                       {addTag(kNull); return *this;}
        Collatable& addBool (bool); // overriding <<(bool) is dangerous due to implicit conversion

        Collatable& operator<< (double);

        Collatable& operator<< (const Collatable&);
        Collatable& operator<< (std::string);
        Collatable& operator<< (const char* cstr)   {return operator<<(slice(cstr));}
        Collatable& operator<< (slice);

        Collatable& beginArray()                    {addTag(kArray); return *this;}
        Collatable& endArray()                      {addTag(kEndSequence); return *this;}

        Collatable& beginMap()                      {addTag(kMap); return *this;}
        Collatable& endMap()                        {addTag(kEndSequence); return *this;}

#ifdef __OBJC__
        Collatable(id obj)                          { if (obj) *this << obj;}
        Collatable& operator<< (id);
#endif

        Collatable& addSpecial()                    {addTag(kSpecial); return *this;}

        operator slice() const                      {return slice(_str);}
        size_t size() const                         {return _str.size();}
        bool empty() const                          {return _str.size() == 0;}
        bool operator< (const Collatable& c) const  {return _str < c._str;}
        bool operator== (const Collatable& c) const {return _str == c._str;}

        std::string dump();

    private:
        void addTag(Tag t)                          {uint8_t c = t; add(slice(&c,1));}
        void add(slice s)                           {_str += std::string((char*)s.buf, s.size);}

        std::string _str;
    };


    /** A decoder of Collatable-format data. Does not own its data (reads from a slice.) */
    class CollatableReader : public CollatableTypes {
    public:
        CollatableReader(slice s) :_data(s) { }

        slice data() const                  {return _data;}
        bool atEnd() const                  {return _data.size == 0;}
        
        Tag peekTag() const;

        int64_t readInt();
        double readDouble();
        alloc_slice readString();

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

        void dumpTo(std::ostream &out);
        std::string dump();

        static uint8_t* getInverseCharPriorityMap();

    private:
        void expectTag(Tag tag);
        void skipTag()                      {_data.moveStart(1);}
        
        slice _data;
    };

}

#endif /* defined(__CBForest__Collatable__) */
