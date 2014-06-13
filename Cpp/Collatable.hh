//
//  Collatable.h
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__Collatable__
#define __CBForest__Collatable__
#include <stdint.h>
#include <string>
#include <iostream>
#include "slice.h"

namespace forestdb {

    class Collatable {
    public:
        Collatable();

        template<typename T> explicit Collatable(const T &t)    {*this << t;}

        Collatable& addNull()                       {addTag(1); return *this;}
        Collatable& addBool (bool); // overriding <<(bool) is dangerous due to implicit conversion

        Collatable& operator<< (int i)              {return *this << (int64_t)i;}
        Collatable& operator<< (int64_t);
        Collatable& operator<< (uint64_t i)         {return *this << (int64_t)i;}

        Collatable& operator<< (const Collatable&);
        Collatable& operator<< (std::string);
        Collatable& operator<< (const char* cstr)   {return operator<<(slice(cstr));}
        Collatable& operator<< (slice);

        Collatable& beginArray()                    {addTag(6); return *this;}
        Collatable& endArray()                      {addTag(0); return *this;}

        Collatable& beginMap()                      {addTag(7); return *this;}
        Collatable& endMap()                        {addTag(0); return *this;}

#ifdef __OBJC__
        Collatable(id obj) {
            if (obj)
                *this << obj;
        }
        Collatable& operator<< (id);
#endif

        operator slice() const                      {return slice(_str);}
        bool empty() const                          {return _str.size() == 0;}
        bool operator< (const Collatable& c) const  {return _str < c._str;}

        std::string dump();

    private:
        void addTag(uint8_t c)                      {add(slice(&c,1));}
        void add(slice s)                           {_str += std::string((char*)s.buf, s.size);}

        std::string _str;
    };


    class CollatableReader {
    public:
        typedef enum {
            kEndSequence = 0,   // Returned to indicate the end of an array/dict
            kNull,
            kFalse,
            kTrue,
            kNumber,
            kString,
            kArray,
            kDictionary,
            kError = 255        // Something went wrong...
        } Tag;

        CollatableReader(slice s) :_data(s) { }

        Tag nextTag() const;

        int64_t readInt();
        alloc_slice readString();

#ifdef __OBJC__
        id readNSObject();
#endif

        /** Reads (skips) an entire object of any type, returning its data in Collatable form. */
        slice read();

        void beginArray();
        void endArray();
        void beginMap();
        void endMap();

        void dumpTo(std::ostream &out);
        std::string dump();

    private:
        void expectTag(uint8_t tag);
        
        slice _data;
    };

}

#endif /* defined(__CBForest__Collatable__) */
